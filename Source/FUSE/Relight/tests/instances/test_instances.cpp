// FUSE Relight RL-1.7: unit tests of instance tracking and the scene model (ctest rl_instances_unit).
//
//   options        rtx.sceneScale / rtx.uniqueObjectDistance are registered here and RL-1.5's by-name reads
//                  (LightOptions::sceneScale, CameraOptions::uniqueObjectDistance) and RL-1.3's config
//                  (applySceneScale -> GeometryCaptureConfig::sceneScale) resolve to them, relight.* twins too.
//   spatialMap     SpatialMap: nearest within the distance across cell borders, filter, exact transform, move,
//                  erase, rebuild.
//   transforms     RtInstance teleport / move / moveAgain history and hasTransformChanged.
//   blasCache      DrawCallCache buckets (single entry reuse / new when touched this frame, scoring), the
//                  build / refit / instance-only decision, GC.
//   tracker        L1 identity (two-pass), L2 exact transform, nearest within uniqueObjectDistance, material
//                  filter, beyond the distance -> new, rebuild on distance change; the identity hash layout.
//   multiInstance  SceneModel on the ff_multi_instance scene (9 cubes x 60 frames: static, moving, spinning,
//                  a teleport past uniqueObjectDistance): ids stable, teleport -> new id, previous transforms
//                  equal the analytic previous frame, the preserve path for unchanged draws, GC.
//   antiCulling    frustum SAT / fast checks, GC keeping out-of-frustum stable objects only when enabled and
//                  not exempt (moving, IgnoreAntiCulling, skinned, camera cut), numObjectsToKeep, the hash.
//   foliage        thin-opaque / SSS instance counts: retain on bind, change, release on GC, option gating.
//   pointInstancer expansion, culling radius, deterministic fade, disabled culling, SceneModel batches.
#include <fuse/relight/scene/instances/instance_options.hpp>
#include <fuse/relight/scene/instances/scene_input.hpp>
#include <fuse/relight/scene/instances/scene_model.hpp>

#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/scene/camera/camera_options.hpp>
#include <fuse/relight/scene/lights/light_options.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;
const char* g_test = "";

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            ++g_failures;                                                                          \
            std::fprintf(stderr, "FAIL %s:%d [%s]: %s\n", __FILE__, __LINE__, g_test, #cond);      \
        }                                                                                          \
    } while (0)

#define CHECK_NEAR(a, b, tol)                                                                                  \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);                              \
        if (!(std::abs(va_ - vb_) <= (tol) * std::max(1.0, std::abs(vb_)))) {                                   \
            ++g_failures;                                                                                      \
            std::fprintf(stderr, "FAIL %s:%d [%s]: %s = %.9g, expected %.9g\n", __FILE__, __LINE__, g_test, #a, \
                         va_, vb_);                                                                            \
        }                                                                                                      \
    } while (0)

using namespace fuse::relight;
using namespace fuse::relight::scene;
using namespace fuse::relight::scene::instances;
// scene (RL-1.5) has its own identityMatrix / multiply: these tests use the instance package's.
using fuse::relight::scene::instances::identityMatrix;
using fuse::relight::scene::instances::multiply;

/// Stand-ins for options other packages own (read by name here; defaults as Remix).
struct FreeCameraStandIn {
    FUSE_RELIGHT_OPTION("rtx.camera", bool, enableFreeCamera, false, "Test stand-in for the camera option.");
};
struct SubsurfaceStandIn {
    FUSE_RELIGHT_OPTION("rtx.subsurface", bool, enableThinOpaque, true, "Test stand-in for the materials option.");
    FUSE_RELIGHT_OPTION("rtx.subsurface", float, surfaceThicknessScale, 1.f, "Test stand-in for the materials option.");
};

/// rtx.conf text applied as an option layer for the scope's lifetime.
class ScopedConf {
public:
    explicit ScopedConf(const std::string& text) {
        static int s_counter = 0;
        const options::OptionConfig config = options::OptionConfig::parse(text);
        m_layer = options::OptionManager::acquireLayer(
            "", {7000u + static_cast<std::uint32_t>(s_counter++), "rl_instances_test"}, 1.0f, 0.1f, false, &config);
        options::OptionManager::applyPendingValues(nullptr, false);
    }
    ~ScopedConf() {
        m_layer = options::OptionLayerHandle();
        options::OptionManager::applyPendingValues(nullptr, false);
    }
    ScopedConf(const ScopedConf&) = delete;
    ScopedConf& operator=(const ScopedConf&) = delete;

private:
    options::OptionLayerHandle m_layer;
};

Mat4f translationM(float x, float y, float z) {
    Mat4f m = identityMatrix();
    m[12] = x;
    m[13] = y;
    m[14] = z;
    return m;
}

Mat4f rotationY(float a) {
    Mat4f m = identityMatrix();
    const float c = std::cos(a), s = std::sin(a);
    m[0] = c;
    m[2] = -s;
    m[8] = s;
    m[10] = c;
    return m;
}

/// A mesh's geometry hashes: `mesh` selects topology + positions, `uv` the texcoords.
hash::GeometryHashes meshHashes(std::uint64_t mesh, std::uint64_t positions = 0, std::uint64_t uv = 7) {
    hash::GeometryHashes h;
    h[hash::HashComponent::Positions] = 0x1000 + mesh * 16 + positions;
    h[hash::HashComponent::Texcoords] = 0x2000 + uv;
    h[hash::HashComponent::Indices] = 0x3000 + mesh;
    h[hash::HashComponent::GeometryDescriptor] = 0x4000 + mesh;
    h[hash::HashComponent::VertexLayout] = 0x5000;
    return h;
}

SceneDrawInput cubeDraw(const Mat4f& world, Hash64 material = 0xabc, std::uint64_t mesh = 1) {
    SceneDrawInput d;
    d.geometry = meshHashes(mesh);
    d.boundingBox.minPos = {-50.f, -50.f, -50.f};
    d.boundingBox.maxPos = {50.f, 50.f, 50.f};
    d.materialHash = material;
    d.materialIdentityHash = material ^ 0x5555;
    d.objectToWorld = world;
    return d;
}

/// A left-handed look-at camera at `eye` looking along +Z (D3DXMatrixLookAtLH with at = eye + z).
CameraState cameraAt(Vec3 eye, float fov = 1.0471975f) {
    CameraState c;
    c.type = CameraType::Main;
    c.fov = fov;
    c.aspectRatio = 4.f / 3.f;
    c.nearPlane = 10.f;
    c.farPlane = 10000.f;
    c.isLHS = true;
    Mat4f v = identityMatrix();
    v[12] = -eye.x;
    v[13] = -eye.y;
    v[14] = -eye.z;
    for (std::size_t i = 0; i < 16; ++i) {
        c.worldToView[i] = v[i];
        c.viewToWorld[i] = (i % 5) == 0 ? 1.0 : 0.0;
    }
    c.viewToWorld[12] = eye.x;
    c.viewToWorld[13] = eye.y;
    c.viewToWorld[14] = eye.z;
    c.previousViewToWorld = c.viewToWorld;
    return c;
}

// ---- options ----------------------------------------------------------------------------------------------------

void test_options() {
    registerInstanceOptions();
    for (const char* name : {"rtx.sceneScale", "rtx.uniqueObjectDistance", "rtx.numFramesToKeepInstances",
                             "rtx.antiCulling.object.enable", "rtx.antiCulling.object.fovScale",
                             "rtx.antiCulling.light.numFramesToExtendLightLifetime", "rtx.pointInstancer.cullingRadius",
                             "rtx.enablePreservePath"}) {
        CHECK(options::OptionManager::findOption(name) != nullptr);
    }
    CHECK(InstanceOptions::sceneScale() == 1.f);
    CHECK(InstanceOptions::uniqueObjectDistance() == 300.f);
    CHECK(LightOptions::sceneScale() == 1.f);
    CHECK(CameraOptions::uniqueObjectDistance() == 300.f);
    {
        ScopedConf conf("rtx.sceneScale = 2.54\nrtx.uniqueObjectDistance = 150\n");
        CHECK(InstanceOptions::sceneScale() == 2.54f);
        CHECK(InstanceOptions::uniqueObjectDistance() == 150.f);
        CHECK(InstanceOptions::uniqueObjectDistanceSqr() == 22500.f);
        CHECK(InstanceOptions::meterToWorldUnitScale() == 254.f);
        // RL-1.5 reads them by name.
        CHECK(LightOptions::sceneScale() == 2.54f);
        CHECK(CameraOptions::uniqueObjectDistance() == 150.f);
        // RL-1.3's geometry config.
        capture::geometry::GeometryCaptureConfig config = capture::geometry::GeometryCaptureConfig::fromOptions();
        applySceneScale(config);
        CHECK(config.sceneScale == 2.54f);
        CHECK(hash::legacyDiscreteStepSize(config.sceneScale) == hash::legacyDiscreteStepSize(2.54f));
    }
    {
        // relight.* twins.
        ScopedConf conf("relight.uniqueObjectDistance = 42\n");
        CHECK(CameraOptions::uniqueObjectDistance() == 42.f);
    }
    {
        // minValue 0.
        ScopedConf conf("rtx.uniqueObjectDistance = -5\n");
        CHECK(InstanceOptions::uniqueObjectDistance() == 0.f);
    }
    CHECK(InstanceOptions::uniqueObjectDistance() == 300.f);
    CHECK(!InstanceOptions::enableFreeCamera());
    CHECK(!AntiCullingOptions::isObjectAntiCullingEnabled());
    CHECK(SubsurfaceOptions::enableThinOpaque() && SubsurfaceOptions::enableDiffusionProfile());
    CHECK(SubsurfaceOptions::surfaceThicknessScale() == 1.f);
    {
        // fadeStartRadius is clamped below cullingRadius (onChange callbacks).
        ScopedConf conf("rtx.pointInstancer.cullingRadius = 100\n");
        ScopedConf conf2("rtx.pointInstancer.fadeStartRadius = 500\n");
        CHECK(PointInstancerOptions::fadeStartRadius() <= 100.f);
    }
}

// ---- spatial map ------------------------------------------------------------------------------------------------

void test_spatialMap() {
    struct D {
        int id;
    };
    D a{1}, b{2}, c{3};
    SpatialMap<D> map(600.f); // uniqueObjectDistance 300
    const Mat4f ta = translationM(0, 0, 0), tb = translationM(290, 0, 0), tc = translationM(-2000, 0, 0);
    const auto ha = map.insert({0, 0, 0}, ta, &a);
    map.insert({290, 0, 0}, tb, &b);
    map.insert({-2000, 0, 0}, tc, &c);
    CHECK(map.size() == 3);
    const auto all = [](const D*) { return true; };
    float d2 = 0;
    CHECK(map.getNearestData({10, 0, 0}, 300.f * 300.f, d2, all) == &a);
    CHECK(d2 == 100.f);
    CHECK(map.getNearestData({280, 0, 0}, 300.f * 300.f, d2, all) == &b);
    // Across a cell border (cells of 600: 299 is in cell 0, the query at 301 looks at cells 0 and 1).
    CHECK(map.getNearestData({301, 0, 0}, 300.f * 300.f, d2, all) == &b);
    // Nothing within the distance.
    CHECK(map.getNearestData({1000, 0, 0}, 300.f * 300.f, d2, all) == nullptr);
    // Filter.
    CHECK(map.getNearestData({280, 0, 0}, 300.f * 300.f, d2, [&](const D* d) { return d != &b; }) == &a);
    // Exact transform.
    int visited = 0;
    map.forEachAtTransform(tb, [&](const D* d) {
        ++visited;
        CHECK(d == &b);
        return false;
    });
    CHECK(visited == 1);
    // Move a: new transform, new cell.
    const auto ha2 = map.move(ha, {1500, 0, 0}, translationM(1500, 0, 0), &a);
    CHECK(ha2 != ha);
    CHECK(map.getNearestData({10, 0, 0}, 300.f * 300.f, d2, [&](const D* d) { return d == &a; }) == nullptr);
    CHECK(map.getNearestData({1490, 0, 0}, 300.f * 300.f, d2, all) == &a);
    map.erase(ha2, &a);
    CHECK(map.size() == 2);
    CHECK(map.getNearestData({1490, 0, 0}, 300.f * 300.f, d2, all) == nullptr);
    // Rebuild with a smaller cell: -2000 is still found.
    map.rebuild(10.f);
    CHECK(map.getNearestData({-2003, 0, 0}, 25.f, d2, all) == &c);
}

// ---- transforms -------------------------------------------------------------------------------------------------

void test_transforms() {
    RtInstance inst(1, 0);
    const Mat4f a = translationM(1, 2, 3), b = translationM(4, 5, 6), c = translationM(7, 8, 9);
    CHECK(!inst.teleport(a));
    CHECK(inst.getTransform() == a && inst.getPrevTransform() == a);
    CHECK(inst.move(b));
    CHECK(inst.getTransform() == b && inst.getPrevTransform() == a);
    CHECK(inst.moveAgain(c));
    CHECK(inst.getTransform() == c && inst.getPrevTransform() == a);
    CHECK(!inst.move(c));
    CHECK(inst.getPrevTransform() == c && inst.getTransform() == c);
    CHECK(inst.teleport(a, b));
    CHECK(inst.getTransform() == a && inst.getPrevTransform() == b);
    // -0 != +0 bytewise (Remix memcmp).
    Mat4f z = identityMatrix();
    Mat4f nz = z;
    nz[12] = -0.0f;
    inst.teleport(z);
    CHECK(inst.move(nz));
    inst.teleportWithHistory(translationM(10, 0, 0));
    CHECK(inst.getTransform()[12] == 10.f && inst.getPrevTransform()[12] == 10.f);
    CHECK(inst.setFrameLastUpdated(5));
    CHECK(!inst.setFrameLastUpdated(5));
    CHECK(inst.registerCamera(CameraType::Main));
    CHECK(!inst.registerCamera(CameraType::Main));
    CHECK(inst.isCameraRegistered(CameraType::Main) && !inst.isCameraRegistered(CameraType::Sky));
    CHECK(inst.setFrameLastUpdated(6));
    CHECK(!inst.isCameraRegistered(CameraType::Main));
    // Normal matrix of a uniform scale 2: 0.5 on the diagonal.
    Mat4f s = identityMatrix();
    s[0] = s[5] = s[10] = 2.f;
    inst.teleport(s);
    CHECK(inst.getNormalObjectToWorld()[0] == 0.5f && inst.getNormalObjectToWorld()[4] == 0.5f);
}

// ---- BLAS cache -------------------------------------------------------------------------------------------------

void test_blasCache() {
    DrawCallCache cache;
    BlasEntry* e1 = nullptr;
    SceneDrawInput d = cubeDraw(translationM(0, 0, 0));
    CHECK(cache.get(d, 0, &e1) == DrawCallCache::CacheState::New);
    CHECK(cache.process(*e1, DrawCallCache::CacheState::New, d, 0) == ObjectCacheState::BuildBVH);
    CHECK(e1->frameLastUpdated == 0 && e1->frameLastTouched == 0 && e1->id == 1);
    // Same frame, exact match: same entry, instance update only.
    BlasEntry* e = nullptr;
    SceneDrawInput d2 = cubeDraw(translationM(100, 0, 0));
    CHECK(cache.get(d2, 0, &e) == DrawCallCache::CacheState::Existed && e == e1);
    CHECK(cache.process(*e, DrawCallCache::CacheState::Existed, d2, 0) == ObjectCacheState::UpdateInstance);
    // Same frame, other material: a second entry in the bucket (the first was touched this frame).
    SceneDrawInput d3 = cubeDraw(translationM(200, 0, 0), 0xdef);
    BlasEntry* e2 = nullptr;
    CHECK(cache.get(d3, 0, &e2) == DrawCallCache::CacheState::New && e2 != e1);
    cache.process(*e2, DrawCallCache::CacheState::New, d3, 0);
    CHECK(cache.size() == 2);
    // Next frame, the bucket has two entries: exact matches are found.
    CHECK(cache.get(d3, 1, &e) == DrawCallCache::CacheState::Existed && e == e2);
    CHECK(cache.get(d, 1, &e) == DrawCallCache::CacheState::Existed && e == e1);
    // Next frame, vertex positions changed (skinned / animated): scored to the closest similar entry, refit.
    SceneDrawInput moved = cubeDraw(translationM(0, 0, 0));
    moved.geometry[hash::HashComponent::Positions] ^= 1;
    CHECK(cache.get(moved, 1, &e) == DrawCallCache::CacheState::Existed && e == e1);
    CHECK(cache.process(*e, DrawCallCache::CacheState::Existed, moved, 1) == ObjectCacheState::UpdateBVH);
    CHECK(e1->previousPositionsDefined && e1->frameLastUpdated == 1);
    // Unchanged next frame: instance only, previous positions released.
    CHECK(cache.get(moved, 2, &e) == DrawCallCache::CacheState::Existed && e == e1);
    CHECK(cache.process(*e, DrawCallCache::CacheState::Existed, moved, 2) == ObjectCacheState::UpdateInstance);
    CHECK(!e1->previousPositionsDefined && e1->frameLastUpdated == 1);
    // Single-entry bucket: an untouched entry is reused by a similar draw (same material).
    DrawCallCache single;
    SceneDrawInput m = cubeDraw(translationM(0, 0, 0), 0x111, 9);
    single.get(m, 0, &e);
    single.process(*e, DrawCallCache::CacheState::New, m, 0);
    BlasEntry* s1 = e;
    SceneDrawInput m2 = m;
    m2.geometry[hash::HashComponent::Positions] ^= 2;
    CHECK(single.get(m2, 1, &e) == DrawCallCache::CacheState::Existed && e == s1);
    // ... but not in the frame it was touched.
    single.process(*e, DrawCallCache::CacheState::Existed, m2, 1);
    SceneDrawInput m3 = m;
    m3.geometry[hash::HashComponent::Positions] ^= 4;
    CHECK(single.get(m3, 1, &e) == DrawCallCache::CacheState::New && e != s1);
    // GC: entries untouched for rtx.numFramesToKeepBLAS (1) frames and without instances.
    cache.garbageCollection(3);
    CHECK(cache.size() == 1); // e1 touched at 2 survives, e2 (touched at 1) is erased
    cache.garbageCollection(4);
    CHECK(cache.size() == 0);
}

// ---- tracker ----------------------------------------------------------------------------------------------------

void test_tracker() {
    DrawCallTracker t;
    TrackerMatch how;
    SceneDrawInput a = cubeDraw(translationM(0, 0, 0));
    ReplacementInstance* ra = t.findOrCreateReplacementInstance(a, 0, &how);
    CHECK(how == TrackerMatch::New && ra->frameCreated == 0);
    ra->frameLastSeen = 0;
    // Two-pass rendering: identical draw in the same frame -> same RI (L1).
    CHECK(t.findOrCreateReplacementInstance(a, 0, &how) == ra && how == TrackerMatch::Identity);
    // A second copy elsewhere in the same frame -> new RI (L2 skips RIs seen this frame).
    SceneDrawInput b = cubeDraw(translationM(100, 0, 0));
    ReplacementInstance* rb = t.findOrCreateReplacementInstance(b, 0, &how);
    CHECK(rb != ra && how == TrackerMatch::New);
    rb->frameLastSeen = 0;
    // Next frame: a moved 50 units -> nearest (a's RI, not b's which is further).
    SceneDrawInput a1 = cubeDraw(translationM(-50, 0, 0));
    CHECK(t.findOrCreateReplacementInstance(a1, 1, &how) == ra && how == TrackerMatch::Nearest);
    CHECK((ra->dirtyFlags & ReplacementInstance::Transform) != 0);
    ra->frameLastSeen = 1;
    ra->objectToWorld = a1.objectToWorld;
    // b unchanged -> identity, drift bits cleared on the first lookup of the frame.
    CHECK(t.findOrCreateReplacementInstance(b, 1, &how) == rb && how == TrackerMatch::Identity);
    CHECK((rb->dirtyFlags & ReplacementInstance::kLookupDriftMask) == 0);
    rb->frameLastSeen = 1;
    // Same transform, different texture transform -> exact transform match (L2), Other dirty.
    SceneDrawInput a2 = a1;
    a2.textureTransform[12] = 0.5f;
    CHECK(t.findOrCreateReplacementInstance(a2, 2, &how) == ra && how == TrackerMatch::ExactTransform);
    CHECK((ra->dirtyFlags & ReplacementInstance::kLookupDriftMask) == ReplacementInstance::Other);
    ra->frameLastSeen = 2;
    // Material filter: a different material at the same place is a new RI.
    SceneDrawInput c = cubeDraw(translationM(-50, 0, 0), 0x999);
    CHECK(t.findOrCreateReplacementInstance(c, 3, &how) != ra && how == TrackerMatch::New);
    // Beyond uniqueObjectDistance (300): new RI; within: nearest (away from b at x = 100).
    SceneDrawInput far = cubeDraw(translationM(-50, 0, -301));
    CHECK(t.findOrCreateReplacementInstance(far, 3, &how) != ra && how == TrackerMatch::New);
    SceneDrawInput within = cubeDraw(translationM(-50 - 299, 0, 0));
    CHECK(t.findOrCreateReplacementInstance(within, 4, &how) == ra && how == TrackerMatch::Nearest);
    // Other mesh (topological bucket): never matched.
    SceneDrawInput other = cubeDraw(translationM(-50 - 299, 0, 0), 0xabc, 2);
    CHECK(t.findOrCreateReplacementInstance(other, 5, &how) != ra && how == TrackerMatch::New);
    // Vertex positions differ at the same transform: nearest, VertexPosHash dirty.
    SceneDrawInput anim = within;
    anim.geometry[hash::HashComponent::Positions] ^= 8;
    CHECK(t.findOrCreateReplacementInstance(anim, 6, &how) == ra && how == TrackerMatch::Nearest);
    CHECK((ra->dirtyFlags & ReplacementInstance::VertexPosHash) != 0);

    // A smaller rtx.uniqueObjectDistance after a rebuild.
    {
        ScopedConf conf("rtx.uniqueObjectDistance = 10\n");
        DrawCallTracker t2;
        SceneDrawInput p = cubeDraw(translationM(0, 0, 0));
        ReplacementInstance* rp = t2.findOrCreateReplacementInstance(p, 0, &how);
        rp->frameLastSeen = 0;
        CHECK(t2.findOrCreateReplacementInstance(cubeDraw(translationM(20, 0, 0)), 1, &how) != rp);
        CHECK(t2.findOrCreateReplacementInstance(cubeDraw(translationM(-9, 0, 0)), 1, &how) == rp);
    }

    // The identity hash: XXH3 over the 168-byte upstream layout.
    SceneDrawInput id = cubeDraw(translationM(1, 2, 3));
    id.boneHash = 0x77;
    id.overrideMaterialHash = 0x88;
    id.categories.set(InstanceCategories::Terrain);
    id.cameraType = CameraType::Sky;
    id.texgenMode = TexGenMode::ViewNormals;
    id.isUsingRaytracedRenderTarget = true;
    unsigned char bytes[168] = {};
    const std::uint64_t head[4] = {id.geometry.hashForRule(hash::rules::kFullGeometry), id.materialHash, id.boneHash,
                                   id.overrideMaterialHash};
    std::memcpy(bytes, head, 32);
    std::memcpy(bytes + 32, id.objectToWorld.data(), 64);
    std::memcpy(bytes + 96, id.textureTransform.data(), 64);
    const std::uint32_t tail[2] = {id.categories.raw(), 4u | (3u << 8) | (1u << 16)};
    std::memcpy(bytes + 160, tail, 8);
    CHECK(DrawCallTracker::computeIdentityHash(id) == hash::xxh3_64(bytes, sizeof bytes));
}

// ---- the ff_multi_instance scene --------------------------------------------------------------------------------

constexpr int kTeleportFrame = 30;

Mat4f multiInstanceWorld(int i, int frame) {
    const float f = float(frame);
    const float x = -600.0f + 300.0f * float(i % 5), z = i < 5 ? 0.0f : 400.0f;
    if (i >= 4 && i <= 6) {
        return translationM(x + 4.0f * f, 3.0f * (float((frame * (i + 1)) % 7) - 3.0f), z - 2.0f * f);
    }
    if (i == 7) {
        return multiply(rotationY(0.1f * f), translationM(x, 0.0f, z));
    }
    if (i == 8) {
        return translationM(frame < kTeleportFrame ? -600.0f : 600.0f, 150.0f, 800.0f);
    }
    return translationM(x, 0.0f, z);
}

void test_multiInstance() {
    SceneModel scene;
    const CameraState camera = cameraAt({0, 500, -1500});
    std::map<int, std::vector<std::uint64_t>> ids; // logical instance -> id per frame
    std::uint32_t preserved = 0;
    for (int frame = 0; frame < 60; ++frame) {
        for (int i = 0; i < 9; ++i) {
            const SceneDrawInput d = cubeDraw(multiInstanceWorld(i, frame), i % 3 == 0 ? 0xb : 0xa);
            const SceneDrawResult r = scene.submitDraw(d);
            ids[i].push_back(r.instanceId);
            CHECK(r.instanceId != 0);
            CHECK(r.objectToWorld == multiInstanceWorld(i, frame));
            const bool fresh = frame == 0 || (i == 8 && frame == kTeleportFrame);
            CHECK(r.created == fresh);
            if (fresh) {
                CHECK(r.prevObjectToWorld == r.objectToWorld); // teleport: treated as still
            } else {
                CHECK(r.prevObjectToWorld == multiInstanceWorld(i, frame - 1));
            }
            // Motion vector of the centroid = analytic motion.
            const Vec3 now = transformPoint(multiInstanceWorld(i, frame), {0, 0, 0});
            const Vec3 before = fresh ? now : transformPoint(multiInstanceWorld(i, frame - 1), {0, 0, 0});
            const Vec3 mv = transformPoint(r.objectToWorld, {0, 0, 0}) - transformPoint(r.prevObjectToWorld, {0, 0, 0});
            CHECK_NEAR(mv.x, (now - before).x, 1e-5);
            CHECK_NEAR(mv.y, (now - before).y, 1e-5);
            CHECK_NEAR(mv.z, (now - before).z, 1e-5);
            if (i < 4 && frame >= 2) {
                CHECK(r.preserved); // unchanged draws take the preserve path
                CHECK(r.isStatic);
            }
            preserved += r.preserved ? 1 : 0;
        }
        const SceneFrameStats s = scene.endFrame(&camera);
        CHECK(s.activeInstances == 9);
        CHECK(s.replacementInstances == 9);
        CHECK(s.createdInstances == (frame == 0 ? 9u : frame == kTeleportFrame ? 1u : 0u));
    }
    CHECK(preserved >= 4 * 58);
    std::set<std::uint64_t> all;
    for (int i = 0; i < 9; ++i) {
        const auto& v = ids[i];
        if (i < 8) {
            CHECK(std::set<std::uint64_t>(v.begin(), v.end()).size() == 1);
            all.insert(v[0]);
        } else {
            CHECK(std::set<std::uint64_t>(v.begin(), v.begin() + kTeleportFrame).size() == 1);
            CHECK(std::set<std::uint64_t>(v.begin() + kTeleportFrame, v.end()).size() == 1);
            CHECK(v[kTeleportFrame] != v[kTeleportFrame - 1]);
            all.insert(v[0]);
            all.insert(v[kTeleportFrame]);
        }
    }
    CHECK(all.size() == 10); // no id reused

    // Without the preserve path the results are the same (static instances: prev == current).
    ScopedConf conf("rtx.enablePreservePath = False\n");
    SceneModel dynamic;
    for (int frame = 0; frame < 3; ++frame) {
        for (int i = 0; i < 9; ++i) {
            const SceneDrawResult r = dynamic.submitDraw(cubeDraw(multiInstanceWorld(i, frame), i % 3 == 0 ? 0xb : 0xa));
            CHECK(!r.preserved);
            CHECK(r.instanceId == std::uint64_t(i + 1));
            if (i < 4) {
                CHECK(r.prevObjectToWorld == r.objectToWorld && r.isStatic && !r.hasTransformChanged);
            }
        }
        dynamic.endFrame(&camera);
    }
}

void test_teleportBoundary() {
    // A draw that moves exactly uniqueObjectDistance keeps its id; a little more gets a new one.
    SceneModel scene;
    const CameraState camera = cameraAt({0, 0, -1000});
    const std::uint64_t id0 = scene.submitDraw(cubeDraw(translationM(0, 0, 0))).instanceId;
    scene.endFrame(&camera);
    CHECK(scene.submitDraw(cubeDraw(translationM(300, 0, 0))).instanceId == id0);
    scene.endFrame(&camera);
    const SceneDrawResult r = scene.submitDraw(cubeDraw(translationM(600.1f, 0, 0)));
    CHECK(r.instanceId != id0 && r.created);
    const SceneFrameStats s = scene.endFrame(&camera);
    CHECK(s.activeInstances == 1 && s.gc.destroyed == 1);
    // Two-pass rendering of one draw: one instance; the second pass does not advance history.
    const SceneDrawInput d = cubeDraw(translationM(610, 0, 0));
    const SceneDrawResult p1 = scene.submitDraw(d);
    const SceneDrawResult p2 = scene.submitDraw(d);
    CHECK(p1.instanceId == p2.instanceId && p2.match == TrackerMatch::Identity);
    CHECK(p2.prevObjectToWorld == translationM(600.1f, 0, 0));
    CHECK(scene.endFrame(&camera).activeInstances == 1);
}

// ---- anti-culling -----------------------------------------------------------------------------------------------

void test_antiCulling() {
    const CameraState camera = cameraAt({0, 0, 0});
    const AntiCullingFrustum f = AntiCullingFrustum::build(camera.fov, camera.aspectRatio, camera.nearPlane, camera.farPlane,
                                                           true, identityMatrix(), 1.f, 10.f, false);
    AxisAlignedBoundingBox box;
    box.minPos = {-1, -1, -1};
    box.maxPos = {1, 1, 1};
    for (bool sat : {false, true}) {
        CHECK(f.intersects(box, translationM(0, 0, 100), sat));      // in front
        CHECK(!f.intersects(box, translationM(0, 0, -100), sat));    // behind
        CHECK(!f.intersects(box, translationM(1000, 0, 100), sat));  // far to the right
        CHECK(!f.intersects(box, translationM(0, 0, 200000), sat));  // beyond 10 x far
        CHECK(f.intersects(box, translationM(0, 0, 50000), sat));    // within 10 x far
    }
    // Just outside the side plane (half fov x ~ 37.6 deg at z = 100: x ~ 77): the box corner at 80 - 1 is out.
    CHECK(!f.intersects(box, translationM(90, 0, 100), true));
    CHECK(f.intersects(box, translationM(70, 0, 100), true));
    // Right-handed camera looks down -Z.
    const AntiCullingFrustum rh = AntiCullingFrustum::build(camera.fov, camera.aspectRatio, 10.f, 10000.f, false,
                                                            identityMatrix(), 1.f, 10.f, false);
    CHECK(rh.intersects(box, translationM(0, 0, -100), false));
    CHECK(!rh.intersects(box, translationM(0, 0, 100), false));

    // GC: an object seen for three frames, then no longer drawn. The camera is off the origin (an identity
    // view-to-world means "no view matrix": anti-culling unsupported).
    const CameraState gcCamera = cameraAt({0, 0, -1});
    const auto run = [&](const std::string& conf, const Mat4f& where, bool moving, bool ignoreCategory, int framesMissing) {
        ScopedConf c(conf);
        SceneModel scene;
        std::uint64_t id = 0;
        for (int frame = 0; frame < 3; ++frame) {
            SceneDrawInput d = cubeDraw(moving ? multiply(where, translationM(float(frame), 0, 0)) : where);
            if (ignoreCategory) {
                d.categories.set(InstanceCategories::IgnoreAntiCulling);
            }
            id = scene.submitDraw(d).instanceId;
            scene.submitDraw(cubeDraw(translationM(0, 0, 100), 0x77)); // a visible object keeps the frame alive
            scene.endFrame(&gcCamera);
        }
        for (int k = 0; k < framesMissing; ++k) {
            scene.submitDraw(cubeDraw(translationM(0, 0, 100), 0x77));
            scene.endFrame(&gcCamera);
        }
        for (const RtInstance* inst : scene.instances().getInstanceTable()) {
            if (inst->getId() == id) {
                return true;
            }
        }
        return false;
    };
    const Mat4f behind = translationM(0, 0, -500), visible = translationM(50, 0, 300);
    CHECK(!run("", behind, false, false, 1));                                  // anti-culling off: collected
    CHECK(run("rtx.antiCulling.object.enable = True\n", behind, false, false, 5)); // out of frustum: kept
    CHECK(run("rtx.antiCulling.object.enable = True\nrtx.antiCulling.object.enableHighPrecisionAntiCulling = False\n",
              behind, false, false, 5));
    CHECK(!run("rtx.antiCulling.object.enable = True\n", visible, false, false, 1)); // in frustum: collected
    CHECK(!run("rtx.antiCulling.object.enable = True\n", behind, true, false, 1));   // moving: exempt
    CHECK(!run("rtx.antiCulling.object.enable = True\n", behind, false, true, 1));   // IgnoreAntiCulling
    CHECK(!run("rtx.antiCulling.object.enable = True\nrtx.antiCulling.object.numObjectsToKeep = 2\n", behind, false,
               false, 1)); // forced GC
    // Free camera disables anti-culling (borrowed rtx.camera.enableFreeCamera).
    CHECK(!run("rtx.antiCulling.object.enable = True\nrtx.camera.enableFreeCamera = True\n", behind, false, false, 1));

    // Camera cut: no anti-culling that frame.
    {
        ScopedConf c("rtx.antiCulling.object.enable = True\n");
        SceneModel scene;
        CameraState cut = gcCamera;
        std::uint64_t id = 0;
        for (int frame = 0; frame < 3; ++frame) {
            id = scene.submitDraw(cubeDraw(behind)).instanceId;
            scene.endFrame(&gcCamera);
        }
        cut.previousViewToWorld[12] = -5000.0; // moved more than uniqueObjectDistance
        CHECK(cut.isCameraCut());
        const SceneFrameStats s = scene.endFrame(&cut);
        CHECK(s.gc.destroyed == 1 && s.activeInstances == 0);
        (void)id;
        // No view matrix set (identity viewToWorld): unsupported, collected.
        SceneModel scene2;
        CameraState none;
        for (std::size_t i = 0; i < 16; ++i) {
            none.viewToWorld[i] = none.previousViewToWorld[i] = (i % 5) == 0 ? 1.0 : 0.0;
        }
        for (int frame = 0; frame < 3; ++frame) {
            scene2.submitDraw(cubeDraw(behind));
            scene2.endFrame(&none);
        }
        const SceneFrameStats s2 = scene2.endFrame(&none);
        CHECK(!s2.antiCullingSupported && s2.activeInstances == 0);
    }

    // The anti-culling hash.
    {
        SceneModel scene;
        scene.submitDraw(cubeDraw(translationM(1, 2, 3), 0x42));
        const RtInstance* inst = scene.instances().getInstanceTable()[0];
        CHECK(inst->calculateAntiCullingHash() == 0);
        ScopedConf c("rtx.antiCulling.object.enable = True\n");
        const float pos[3] = {1, 2, 3};
        const Hash64 mat = 0x42;
        Hash64 expect = hash::xxh3_64(&mat, 8, hash::xxh3_64(pos, 12));
        const float bb[6] = {-50, -50, -50, 50, 50, 50};
        const Hash64 bbh = hash::xxh3_64(bb, 24);
        CHECK(inst->calculateAntiCullingHash() == hash::xxh3_64(&bbh, 8, expect));
        ScopedConf c2("rtx.antiCulling.object.hashInstanceWithBoundingBoxHash = False\n");
        CHECK(inst->calculateAntiCullingHash() == expect);
    }
}

// ---- foliage ----------------------------------------------------------------------------------------------------

void test_foliage() {
    const CameraState camera = cameraAt({0, 0, -1000});
    SceneModel scene;
    SceneDrawInput leaf = cubeDraw(translationM(0, 0, 0), 0x1);
    leaf.subsurface.measurementDistance = 2.f;
    SceneDrawInput skin = cubeDraw(translationM(500, 0, 0), 0x2);
    skin.subsurface.diffusionProfile = true;
    SceneDrawInput plain = cubeDraw(translationM(1000, 0, 0), 0x3);
    SceneDrawInput glass = cubeDraw(translationM(1500, 0, 0), 0x4);
    glass.materialType = MaterialType::Translucent;
    glass.subsurface.measurementDistance = 1.f; // not opaque: no subsurface
    for (int frame = 0; frame < 3; ++frame) {
        scene.submitDraw(leaf);
        scene.submitDraw(skin);
        scene.submitDraw(plain);
        scene.submitDraw(glass);
        const SceneFrameStats s = scene.endFrame(&camera);
        CHECK(s.thinOpaqueInstances == 1 && s.sssInstances == 1);
    }
    CHECK(scene.isThinOpaqueMaterialExist());
    const RtInstance* sssInstance = nullptr;
    for (const RtInstance* inst : scene.instances().getInstanceTable()) {
        if (inst->getSubsurfaceKind() == SubsurfaceKind::DiffusionProfile) {
            sssInstance = inst;
        }
    }
    CHECK(sssInstance != nullptr && sssInstance->isSubsurface());
    // The leaf's material loses its subsurface (a material change): released.
    SceneDrawInput bare = leaf;
    bare.subsurface = SubsurfaceInput();
    bare.materialIdentityHash ^= 1;
    scene.submitDraw(bare);
    scene.submitDraw(skin);
    CHECK(scene.endFrame(&camera).thinOpaqueInstances == 0);
    // The SSS object stops being drawn: released when collected.
    scene.submitDraw(bare);
    SceneFrameStats s = scene.endFrame(&camera);
    CHECK(s.sssInstances == 0 && s.thinOpaqueInstances == 0);
    CHECK(scene.subsurfaceUnderflows() == 0);
    // Option gating.
    CHECK(classifySubsurface({2.f, false}) == SubsurfaceKind::ThinOpaque);
    CHECK(classifySubsurface({0.f, true}) == SubsurfaceKind::DiffusionProfile);
    CHECK(classifySubsurface({0.f, false}) == SubsurfaceKind::None);
    {
        ScopedConf c("rtx.subsurface.enableThinOpaque = False\n");
        CHECK(classifySubsurface({2.f, false}) == SubsurfaceKind::None);
    }
    {
        ScopedConf c("rtx.subsurface.surfaceThicknessScale = 0\n");
        CHECK(classifySubsurface({2.f, false}) == SubsurfaceKind::None);
    }
    // The legacy material identity hash follows the material record.
    LegacyMaterialRecord m;
    const Hash64 h0 = legacyMaterialIdentityHash(m);
    m.tFactor = 0x80808080u;
    CHECK(legacyMaterialIdentityHash(m) != h0);
    m.tFactor = 0xffffffffu;
    CHECK(legacyMaterialIdentityHash(m) == h0);
    m.d3dMaterial.power = 5.f; // excluded
    CHECK(legacyMaterialIdentityHash(m) == h0);
}

// ---- point instancer --------------------------------------------------------------------------------------------

void test_pointInstancer() {
    auto transforms = std::make_shared<std::vector<Mat4f>>();
    for (int k = 0; k < 100; ++k) {
        transforms->push_back(translationM(float(k) * 100.f, 0, 0));
    }
    PointInstancerBatch batch;
    batch.transforms = transforms;
    batch.objectToWorld = translationM(0, 0, 10);
    batch.prevObjectToWorld = translationM(0, 0, 5);
    batch.baseSurfaceIndex = 7;
    PointInstancerCulling cull;
    cull.cameraPosition = {0, 0, 0};
    cull.cullingRadius = 5000.f;
    cull.fadeStartRadius = 0.f;
    std::vector<PointInstance> out = expandPointInstancer(batch, cull);
    CHECK(out.size() == 100);
    CHECK(out[3].objectToWorld == translationM(300, 0, 10));
    CHECK(out[3].prevObjectToWorld == translationM(300, 0, 5));
    CHECK(out[3].surfaceIndex == 10);
    int visible = 0;
    for (const PointInstance& p : out) {
        visible += p.visible() ? 1 : 0;
    }
    CHECK(visible == 50); // k * 100 at distance sqrt((100k)^2 + 100) <= 5000: k = 0..49
    // Fade: deterministic and monotone in density.
    cull.fadeStartRadius = 1000.f;
    const std::vector<PointInstance> faded = expandPointInstancer(batch, cull);
    const std::vector<PointInstance> faded2 = expandPointInstancer(batch, cull);
    int fadedVisible = 0;
    for (std::size_t k = 0; k < faded.size(); ++k) {
        CHECK(faded[k].mask == faded2[k].mask);
        fadedVisible += faded[k].visible() ? 1 : 0;
        if (k <= 9) {
            CHECK(faded[k].visible());
        }
        // The shader's rejection: rand01 < (dist - fadeStart) / fadeRange.
        const float dist = std::sqrt(float(k * 100) * float(k * 100) + 100.f);
        if (dist > 1000.f && dist <= 5000.f) {
            const std::uint32_t h = std::uint32_t(k) * 2654435761u;
            const float rand01 = float(h & 0xFFFFu) / 65535.0f;
            CHECK(faded[k].visible() == !(rand01 < (dist - 1000.f) / 4000.f));
        }
    }
    CHECK(fadedVisible < visible && fadedVisible > 10);
    // Options: disabled culling keeps everything.
    {
        ScopedConf c("rtx.pointInstancer.enable = False\n");
        const PointInstancerCulling all = PointInstancerCulling::fromOptions({0, 0, 0});
        int n = 0;
        for (const PointInstance& p : expandPointInstancer(batch, all)) {
            n += p.visible() ? 1 : 0;
        }
        CHECK(n == 100);
    }
    // SceneModel: an instancer draw next to a plain draw; slots follow the instance table.
    SceneModel scene;
    const CameraState camera = cameraAt({0, 0, 0});
    SceneDrawInput plain = cubeDraw(translationM(0, 0, 100), 0x1);
    SceneDrawInput inst = cubeDraw(translationM(0, 0, 10), 0x2, 3);
    inst.instancesToObject = transforms;
    inst.subsurface.diffusionProfile = true;
    scene.submitDraw(plain);
    scene.submitDraw(inst);
    SceneFrameStats s = scene.endFrame(&camera);
    CHECK(scene.pointInstancerBatches().size() == 2); // + the SSS batch
    CHECK(scene.pointInstancerBatches()[0].baseSurfaceIndex == 1 && scene.pointInstancerBatches()[1].subsurfaceTlas);
    CHECK(s.pointInstances == 100 && s.visiblePointInstances == 50);
    // Next frame the instancer moved: prev transforms come from the instance history.
    inst.objectToWorld = translationM(0, 0, 20);
    scene.submitDraw(plain);
    scene.submitDraw(inst);
    scene.endFrame(&camera);
    CHECK(scene.pointInstances()[5].objectToWorld == translationM(500, 0, 20));
    CHECK(scene.pointInstances()[5].prevObjectToWorld == translationM(500, 0, 10));
}

struct TestCase {
    const char* name;
    void (*fn)();
};

} // namespace

int main() {
    options::setEnvironmentVariable(options::kDxvkConfEnvVar, "");
    options::setEnvironmentVariable(options::kRtxConfEnvVar, "");
    options::setEnvironmentVariable("RTX_ANTI_CULLING_OBJECTS", "");
    options::setEnvironmentVariable("RTX_ANTI_CULLING_LIGHTS", "");
    registerInstanceOptions();
    // odr-use: registers the stand-in options.
    (void)FreeCameraStandIn::enableFreeCameraObject();
    (void)SubsurfaceStandIn::enableThinOpaqueObject();
    (void)SubsurfaceStandIn::surfaceThicknessScaleObject();
    options::OptionManager::applyPendingValues(nullptr, false);

    const TestCase tests[] = {
        {"options", test_options},         {"spatialMap", test_spatialMap},       {"transforms", test_transforms},
        {"blasCache", test_blasCache},     {"tracker", test_tracker},             {"multiInstance", test_multiInstance},
        {"teleportBoundary", test_teleportBoundary}, {"antiCulling", test_antiCulling}, {"foliage", test_foliage},
        {"pointInstancer", test_pointInstancer},
    };
    for (const TestCase& t : tests) {
        g_test = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("%s %s\n", g_failures == before ? "ok  " : "FAIL", t.name);
    }
    std::printf("rl_instances_unit: %d check(s), %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
