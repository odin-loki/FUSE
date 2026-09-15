#include <fuse/scene/camera.hpp>

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

void expectNear(float actual, float expected, float epsilon, const char* message) {
    if (std::fabs(actual - expected) > epsilon) {
        std::fprintf(stderr, "FAIL: %s (got %f expected %f)\n", message, actual, expected);
        ++g_failures;
    }
}

void testCameraUpdate() {
    fuse::Camera camera;
    camera.setPosition(0.f, 2.f, 10.f);
    camera.setOrientation(90.f, -10.f);
    camera.fovDeg = 60.f;
    camera.aspectRatio = 1.777f;
    camera.nearPlane = 0.5f;
    camera.farPlane = 500.f;
    camera.update();

    expectTrue(camera.matricesValid(), "camera matrices marked valid after update");
    expectNear(camera.viewMatrixRow(3, 3), 1.f, 0.001f, "view matrix homogeneous row");

    const float proj00 = camera.projectionMatrixRow(0, 0);
    expectTrue(proj00 > 0.f, "projection matrix non-degenerate");
    expectTrue(camera.frustum().planes[0][0] != 0.f || camera.frustum().planes[0][1] != 0.f ||
                   camera.frustum().planes[0][2] != 0.f,
               "frustum left plane extracted");
}

void testCameraOrientationRoundTrip() {
    fuse::Camera camera;
    camera.setPosition(3.f, 4.f, 5.f);
    camera.setOrientation(45.f, 15.f);
    camera.update();

    expectNear(camera.positionX, 3.f, 0.001f, "camera position x preserved");
    expectNear(camera.positionY, 4.f, 0.001f, "camera position y preserved");
    expectNear(camera.positionZ, 5.f, 0.001f, "camera position z preserved");
    expectNear(camera.yawDeg, 45.f, 0.001f, "camera yaw preserved");
    expectNear(camera.pitchDeg, 15.f, 0.001f, "camera pitch preserved");
}

void testCameraProjectionHelpers() {
    fuse::Camera camera;
    camera.setPosition(0.f, 0.f, 10.f);
    camera.setOrientation(180.f, 0.f);
    camera.fovDeg = 90.f;
    camera.aspectRatio = 1.f;
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    camera.update();

    expectTrue(camera.matricesValid(), "projection helpers require valid matrices");
    expectTrue(camera.viewProjectionMatrixRow(3, 3) != 0.f, "view-projection matrix populated");

    const fuse::Camera::ProjectedPoint origin = camera.projectWorldPoint(0.f, 0.f, 0.f);
    expectTrue(!origin.behindCamera, "world origin in front of camera");
    expectTrue(origin.clipW > 0.f, "clip w positive for visible point");

    float ndcX = 0.f;
    float ndcY = 0.f;
    float ndcZ = 0.f;
    expectTrue(camera.worldToNdc(0.f, 0.f, 0.f, ndcX, ndcY, ndcZ), "worldToNdc succeeds for origin");
    expectNear(ndcX, 0.f, 0.05f, "origin projects near ndc center x");
    expectNear(ndcY, 0.f, 0.05f, "origin projects near ndc center y");

    const fuse::Camera::ProjectedPoint behind = camera.projectWorldPoint(0.f, 0.f, 20.f);
    expectTrue(behind.behindCamera, "point behind camera flagged");
    expectTrue(!camera.worldToNdc(0.f, 0.f, 20.f, ndcX, ndcY, ndcZ), "worldToNdc rejects behind camera");
}

void testCameraProjectionOffCenter() {
    fuse::Camera camera;
    camera.setPosition(0.f, 0.f, 10.f);
    camera.setOrientation(180.f, 0.f);
    camera.fovDeg = 90.f;
    camera.aspectRatio = 1.f;
    camera.nearPlane = 1.f;
    camera.farPlane = 100.f;
    camera.update();

    float ndcX = 0.f;
    float ndcY = 0.f;
    float ndcZ = 0.f;
    expectTrue(camera.worldToNdc(2.f, 0.f, 0.f, ndcX, ndcY, ndcZ), "offset point projects");
    expectTrue(ndcX > 0.1f, "positive x world point lands right of center in ndc");
}

} // namespace

int test_camera_main() {
    testCameraUpdate();
    testCameraOrientationRoundTrip();
    testCameraProjectionHelpers();
    testCameraProjectionOffCenter();
    return g_failures;
}
