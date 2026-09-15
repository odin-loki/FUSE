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

void testMat4RigidGuards() {
    const fuse::math::Mat4 rotation =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 0.6f), {1.f, 1.f, 1.f});
    expectTrue(fuse::math::isOrthogonalUpper3x3(rotation), "Mat4 pure rotation is orthogonal upper 3x3");
    expectTrue(fuse::math::isRigidUpper3x3(rotation), "Mat4 pure rotation is rigid upper 3x3");

    const fuse::math::Mat4 uniformScale =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::Quat::identity(), {2.f, 2.f, 2.f});
    expectTrue(!fuse::math::isOrthogonalUpper3x3(uniformScale),
               "Mat4 uniform scale is not unit-length orthogonal");
    expectTrue(fuse::math::isRigidUpper3x3(uniformScale), "Mat4 uniform scale is rigid upper 3x3");

    const fuse::math::Mat4 nonUniformScale =
        fuse::math::fromTRS({0.f, 0.f, 0.f}, fuse::math::Quat::identity(), {2.f, 3.f, 4.f});
    expectTrue(!fuse::math::isRigidUpper3x3(nonUniformScale),
               "Mat4 non-uniform scale fails rigid upper 3x3 guard");

    fuse::math::Mat4 inverse{};
    expectTrue(fuse::math::tryInverseAffine(rotation, inverse), "tryInverseAffine accepts rotation");
    expectMat4Near(inverse, fuse::math::inverseAffine(rotation), 1e-4f,
                   "tryInverseAffine matches inverseAffine for rotation");

    expectTrue(!fuse::math::tryInverseAffine(uniformScale, inverse),
               "tryInverseAffine rejects uniform scale");
    expectTrue(!fuse::math::tryInverseAffine(nonUniformScale, inverse),
               "tryInverseAffine rejects non-uniform scale");
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

    const f32 inside = box.rayIntersect({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f});
    expectNear(inside, 0.f, 1e-5f, "AABB ray from interior returns forward entry at zero");

    const f32 behind = box.rayIntersect({0.f, 0.f, 0.f}, {-1.f, 0.f, 0.f});
    expectNear(behind, 0.f, 1e-5f, "AABB ray from interior along -X returns exit distance");

    expectNear(box.rayIntersect({0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}), 0.f, 1e-5f,
               "AABB zero-direction ray inside box returns zero");
    expectNear(box.rayIntersect({3.f, 0.f, 0.f}, {0.f, 0.f, 0.f}), -1.f, 1e-5f,
               "AABB zero-direction ray outside box misses");
}

void testAabbEmptyEdgeCases() {
    const fuse::math::AABB inverted{{1.f, 1.f, 1.f}, {0.f, 0.f, 0.f}};
    expectTrue(inverted.isEmpty(), "AABB inverted bounds are empty");
    expectTrue(!inverted.isValid(), "AABB inverted bounds are invalid");

    const fuse::math::AABB point{{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f}};
    expectTrue(!point.isEmpty(), "AABB zero-volume point box is not empty");
    expectTrue(point.isValid(), "AABB zero-volume point box is valid");
    expectTrue(point.contains({0.f, 0.f, 0.f}), "AABB point box contains its corner");

    expectNear(inverted.rayIntersect({0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}), -1.f, 1e-5f,
               "AABB empty box ray always misses");

    const fuse::math::AABB valid{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    expectAabbNear(inverted.merge(valid), valid, 1e-5f, "AABB merge empty with valid returns valid");
    expectAabbNear(valid.merge(inverted), valid, 1e-5f, "AABB merge valid with empty returns valid");
    expectTrue(inverted.merge(inverted).isEmpty(), "AABB merge two empty boxes stays empty");
    expectTrue(!inverted.overlaps(valid), "AABB empty does not overlap valid box");

    const fuse::math::Mat4 translate =
        fuse::math::fromTRS({3.f, 0.f, 0.f}, fuse::math::Quat::identity(), {1.f, 1.f, 1.f});
    expectTrue(fuse::math::transformAabb(translate, inverted).isEmpty(),
               "AABB transformAabb preserves empty");
    expectTrue(fuse::math::transformAabbCorners(translate, inverted).isEmpty(),
               "AABB transformAabbCorners preserves empty");
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

    const fuse::math::AABB scaledEnvelope = fuse::math::transformAabb(scaled, local);
    expectTrue(scaledEnvelope.min.x <= scaledExact.min.x && scaledEnvelope.min.y <= scaledExact.min.y &&
                   scaledEnvelope.min.z <= scaledExact.min.z,
               "AABB transformAabb envelope lower bounds contain exact corners");
    expectTrue(scaledEnvelope.max.x >= scaledExact.max.x && scaledEnvelope.max.y >= scaledExact.max.y &&
                   scaledEnvelope.max.z >= scaledExact.max.z,
               "AABB transformAabb envelope upper bounds contain exact corners");
}

void testAabbMergeFreeFunction() {
    const fuse::math::AABB a{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const fuse::math::AABB b{{0.5f, 0.5f, 0.5f}, {2.f, 2.f, 2.f}};
    expectAabbNear(fuse::math::mergeAabb(a, b), a.merge(b), 1e-5f, "mergeAabb free function matches member");
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

void testSimdBackend() {
#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
    expectTrue(fuse::math::simd::hasSseBackend(), "simd SSE backend enabled on SSE2 targets");
#else
    expectTrue(!fuse::math::simd::hasSseBackend(), "simd scalar backend on non-SSE targets");
#endif
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

void testPlaneDegenerate() {
    const fuse::math::Vec4 degenerate{0.f, 0.f, 0.f, 1.f};
    expectTrue(fuse::math::isDegeneratePlane(degenerate), "Plane zero normal is degenerate");

    expectTrue(fuse::math::classifyPoint(degenerate, {1.f, 2.f, 3.f}) == fuse::math::PlaneSide::On,
               "Plane classifyPoint treats degenerate plane as on");

    const fuse::math::AABB box{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    expectTrue(fuse::math::classifyAabb(degenerate, box) == fuse::math::PlaneSide::Straddling,
               "Plane classifyAabb treats degenerate plane as straddling");

    fuse::math::Vec3 a{-1.f, 0.f, 0.f};
    fuse::math::Vec3 b{1.f, 0.f, 0.f};
    expectTrue(fuse::math::clipSegmentAgainstPlane(degenerate, a, b),
               "Plane clip keeps segment against degenerate plane");

    const fuse::math::Vec3 tri[3] = {{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}};
    fuse::math::Vec3 out[3]{};
    expectTrue(fuse::math::clipPolygonAgainstPlane(degenerate, tri, 3, out, 3) == 3,
               "Plane polygon clip passes through on degenerate plane");
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

    const fuse::math::AABB empty{{2.f, 2.f, 2.f}, {1.f, 1.f, 1.f}};
    expectTrue(fuse::math::classifyAabb(plane, empty) == fuse::math::PlaneSide::Behind,
               "Plane classifyAabb treats empty AABB as culled");
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

    const fuse::math::Vec3 behind[4] = {
        {-2.f, -1.f, 0.f},
        {-1.f, -1.f, 0.f},
        {-1.f, 1.f, 0.f},
        {-2.f, 1.f, 0.f},
    };
    fuse::math::Vec3 culled[8]{};
    expectTrue(fuse::math::clipPolygonAgainstPlane(plane, behind, 4, culled, 8) == 0,
               "Plane polygon clip culls fully behind quad");
    expectTrue(fuse::math::clipPolygonAgainstPlane(plane, behind, 0, culled, 8) == 0,
               "Plane polygon clip rejects zero input");

    fuse::math::Vec3 inFrontA{1.f, 0.f, 0.f};
    fuse::math::Vec3 inFrontB{3.f, 0.f, 0.f};
    const fuse::math::Vec3 inFrontAExpected = inFrontA;
    const fuse::math::Vec3 inFrontBExpected = inFrontB;
    expectTrue(fuse::math::clipSegmentAgainstPlane(plane, inFrontA, inFrontB),
               "Plane clip keeps segment fully in front");
    expectVec3Near(inFrontA, inFrontAExpected, 1e-5f, "Plane clip in-front segment start unchanged");
    expectVec3Near(inFrontB, inFrontBExpected, 1e-5f, "Plane clip in-front segment end unchanged");
}

void testSimdOrthonormalizeEdgeCases() {
    const fuse::math::simd::Mat4 identity = fuse::math::simd::Mat4::identity();
    expectTrue(fuse::math::simd::isOrthogonalUpper3x3(identity), "simd identity is orthogonal");
    const fuse::math::simd::Mat4 orthoIdentity = fuse::math::simd::orthonormalize(identity);
    expectMat4Near(orthoIdentity.toScalar(), identity.toScalar(), 1e-5f,
                   "simd orthonormalize leaves identity unchanged");

    fuse::math::simd::Mat4 nearSingular = fuse::math::simd::Mat4::identity();
    nearSingular.cols[0] = {1e-6f, 0.f, 0.f, 0.f};
    nearSingular.cols[1] = {0.f, 1e-6f, 0.f, 0.f};
    nearSingular.cols[2] = {0.f, 0.f, 1e-6f, 0.f};
    const fuse::math::simd::Mat4 recovered = fuse::math::simd::orthonormalize(nearSingular);
    expectTrue(fuse::math::simd::isOrthogonalUpper3x3(recovered),
               "simd orthonormalize recovers orthogonal basis from near-singular input");
}

void testSimdAabbEmpty() {
    const fuse::math::AABB empty{{2.f, 2.f, 2.f}, {1.f, 1.f, 1.f}};
    const fuse::math::simd::Mat4 matrix = fuse::math::simd::Mat4::identity();

    expectTrue(fuse::math::simd::transformAabb(matrix, empty).isEmpty(),
               "simd transformAabb preserves empty");
    expectNear(fuse::math::simd::rayIntersectAabb(empty, {0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}), -1.f, 1e-5f,
               "simd rayIntersectAabb rejects empty box");

    const fuse::math::AABB valid{{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    expectAabbNear(fuse::math::simd::mergeAabb(empty, valid), valid, 1e-5f,
                   "simd mergeAabb empty with valid returns valid");
}

void testSimdPlaneParity() {
    const fuse::math::Vec4 plane{0.f, 1.f, 0.f, -2.f};
    const fuse::math::AABB crossing{{0.f, 1.f, 0.f}, {1.f, 3.f, 1.f}};

    expectTrue(fuse::math::simd::classifyPoint(plane, {0.f, 3.f, 0.f}) ==
                   fuse::math::classifyPoint(plane, {0.f, 3.f, 0.f}),
               "simd classifyPoint matches scalar");
    expectNear(fuse::math::simd::planeSignedDistance(plane, {0.f, 2.f, 0.f}),
               fuse::math::planeSignedDistance(plane, {0.f, 2.f, 0.f}), 1e-5f,
               "simd planeSignedDistance matches scalar");
    expectTrue(fuse::math::simd::isDegeneratePlane({0.f, 0.f, 0.f, 0.f}),
               "simd isDegeneratePlane matches scalar degenerate test");

    expectTrue(fuse::math::simd::classifyAabb(plane, crossing) == fuse::math::classifyAabb(plane, crossing),
               "simd classifyAabb matches scalar");

    fuse::math::Vec3 a{-2.f, 0.f, 0.f};
    fuse::math::Vec3 b{2.f, 3.f, 0.f};
    fuse::math::Vec3 simdA = a;
    fuse::math::Vec3 simdB = b;
    const bool scalarClip = fuse::math::clipSegmentAgainstPlane(plane, a, b);
    const bool simdClip = fuse::math::simd::clipSegmentAgainstPlane(plane, simdA, simdB);
    expectTrue(scalarClip == simdClip, "simd clipSegmentAgainstPlane hit/miss matches scalar");
    expectVec3Near(simdA, a, 1e-5f, "simd clipSegmentAgainstPlane start matches scalar");
    expectVec3Near(simdB, b, 1e-5f, "simd clipSegmentAgainstPlane end matches scalar");
}

void testSimdPlanePolygonParity() {
    const fuse::math::Vec4 plane{1.f, 0.f, 0.f, 0.f};
    const fuse::math::Vec3 halfSquare[4] = {
        {-2.f, -1.f, 0.f},
        {2.f, -1.f, 0.f},
        {2.f, 1.f, 0.f},
        {-2.f, 1.f, 0.f},
    };

    fuse::math::Vec3 scalarClipped[8]{};
    fuse::math::Vec3 simdClipped[8]{};
    const u32 scalarCount =
        fuse::math::clipPolygonAgainstPlane(plane, halfSquare, 4, scalarClipped, 8);
    const u32 simdCount =
        fuse::math::simd::clipPolygonAgainstPlane(plane, halfSquare, 4, simdClipped, 8);
    expectTrue(scalarCount == simdCount, "simd clipPolygonAgainstPlane count matches scalar");
    for (u32 i = 0; i < scalarCount; ++i) {
        expectVec3Near(simdClipped[i], scalarClipped[i], 1e-4f, "simd clipPolygonAgainstPlane vertex matches scalar");
    }

    const fuse::math::AABB empty{{1.f, 1.f, 1.f}, {0.f, 0.f, 0.f}};
    expectTrue(fuse::math::simd::classifyAabb(plane, empty) == fuse::math::PlaneSide::Behind,
               "simd classifyAabb treats empty AABB as culled");
}

void testSimdMat4Associativity() {
    const fuse::math::Mat4 a =
        fuse::math::fromTRS({1.f, 0.f, 0.f}, fuse::math::fromAxisAngle({0.f, 1.f, 0.f}, 0.3f), {1.f, 1.f, 1.f});
    const fuse::math::Mat4 b =
        fuse::math::fromTRS({0.f, 2.f, 0.f}, fuse::math::fromAxisAngle({1.f, 0.f, 0.f}, -0.5f), {1.f, 1.f, 1.f});
    const fuse::math::Mat4 c =
        fuse::math::fromTRS({0.f, 0.f, 3.f}, fuse::math::fromAxisAngle({0.f, 0.f, 1.f}, 1.1f), {1.f, 1.f, 1.f});

    const fuse::math::simd::Mat4 simdA = fuse::math::simd::Mat4::fromScalar(a);
    const fuse::math::simd::Mat4 simdB = fuse::math::simd::Mat4::fromScalar(b);
    const fuse::math::simd::Mat4 simdC = fuse::math::simd::Mat4::fromScalar(c);

    const fuse::math::Mat4 scalarAssoc = (a * b) * c;
    const fuse::math::Mat4 simdAssoc = fuse::math::simd::multiply(fuse::math::simd::multiply(simdA, simdB), simdC)
                                           .toScalar();
    expectMat4Near(simdAssoc, scalarAssoc, 1e-4f, "simd Mat4 multiply associativity matches scalar");
}

} // namespace

int main() {
    testVecBasics();
    testMat4MultiplyIdentity();
    testMat4MultiplyEdgeCases();
    testMat4TransformPoint();
    testMat4InverseAffine();
    testMat4RigidGuards();
    testMat4InverseEdgeCases();
    testMat3Upper3x3();
    testQuatRotation();
    testQuatSlerp();
    testAabbOverlap();
    testAabbRayIntersect();
    testAabbEmptyEdgeCases();
    testAabbTransformHelpers();
    testAabbMergeFreeFunction();
    testFrustumCulling();
    testSdfPrimitives();
    testSimdBackend();
    testSimdMat4Parity();
    testSimdMat4InverseEdgeCases();
    testSimdMat4Associativity();
    testSimdOrthonormalizeEdgeCases();
    testSimdAabbStubs();
    testSimdAabbEmpty();
    testSimdPlaneParity();
    testSimdPlanePolygonParity();
    testPlaneDegenerate();
    testPlaneClassify();
    testPlaneClip();

    if (g_failures != 0) {
        std::fprintf(stderr, "%d math test(s) failed.\n", g_failures);
        return EXIT_FAILURE;
    }

    std::fprintf(stdout, "fuse_core_math_tests: all tests passed.\n");
    return EXIT_SUCCESS;
}
