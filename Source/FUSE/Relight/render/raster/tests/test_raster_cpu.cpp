// FUSE Relight RL-4.2: CPU gates of the raster remaster's frame builder (raster_scene.hpp), no Vulkan.
//
//   tiers     planTier: the T0 / T1 / T2 feature masks, the device / option / FUSE_RENDER_TIER_MAX caps and graceful
//             degradation (features this build lacks, down to G-buffer + clustered);
//   material  the legacy material mapping (D3D material colours, the never-set default, COLOR0 as diffuse, alpha
//             test, specular power -> roughness, sky unlit) and the matrix helpers (inverse-transpose normals);
//   build     a synthetic frame: buckets (opaque / sky / decal / blend / skipped), de-indexing of lists, strips (D3D
//             winding) and fans, the blend order (submission), fog selection, the directional list, the clustered
//             light assignment (a light right of the camera lands in the right half of the grid: the mirrored
//             handedness fix), the fallback light, the shadow camera (casters inside its clip volume).
// Usage: fuse_relight_raster_tests <suite>. Exit 0 pass, 1 fail.
#include <fuse/relight/render/raster/raster_options.hpp>
#include <fuse/relight/render/raster/raster_scene.hpp>

#include <fuse/relight/scene/classify/instance_categories.hpp>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace rr = fuse::relight::render::raster;
namespace rt = fuse::relight::tap;
namespace sc = fuse::relight::scene;
namespace geo = fuse::relight::capture::geometry;
namespace gs = fuse::renderer::gpu_scene;

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what.c_str());
    }
}

bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps; }

void testTiers() {
    const std::uint32_t all = rr::implementedFeatures();
    rr::TierPlan p = rr::planTier(2, -1, all);
    check(p.tier == 2, "device T2, auto -> T2");
    check((p.enabled & rr::kFeatureShadows) && (p.enabled & rr::kFeatureForward) && (p.enabled & rr::kFeatureFog) &&
              (p.enabled & rr::kFeatureDecals),
          "T2 enables T0 + T1 features this build has");
    check(p.degraded == "meshlet_path,rt_shadows,rt_reflections,ddgi",
          "T2 degrades the absent packages (" + p.degraded + ")");
    p = rr::planTier(2, 1, all);
    check(p.tier == 1 && (p.enabled & rr::kFeatureShadows) && p.degraded == "meshlet_path", "relight.raster.tier = t1");
    p = rr::planTier(2, 0, all);
    check(p.tier == 0 && !(p.enabled & rr::kFeatureShadows) && p.degraded.empty(), "T0: nothing degraded");
    p = rr::planTier(0, 2, all);
    check(p.tier == 0, "the device tier caps the option");
    // A build (or device) without the optional passes: down to G-buffer + clustered.
    p = rr::planTier(2, -1, 0);
    check(p.enabled == (rr::kFeatureGBuffer | rr::kFeatureClustered), "minimum: G-buffer + clustered");
    check(p.degraded.find("forward") != std::string::npos && p.degraded.find("shadows") != std::string::npos,
          "every missing feature reported (" + p.degraded + ")");
    check(rr::featureList(rr::tierFeatures(0)) == "gbuffer,clustered,forward,decals,fog", "T0 feature list");
    int t = 7;
    check(rr::parseRasterTier("T1", t) && t == 1 && rr::parseRasterTier("auto", t) && t == -1 &&
              !rr::parseRasterTier("t3", t),
          "relight.raster.tier parsing");
}

void testMaterial() {
    sc::LegacyMaterialRecord m;
    rr::RasterMaterial r = rr::legacyMaterial(m, false, false);
    check(r.diffuse[0] == 1.f && r.diffuse[3] == 1.f, "never-set D3D material: opaque white diffuse");
    m.d3dMaterial.diffuse = {0.9f, 0.8f, 0.7f, 0.5f};
    m.d3dMaterial.emissive = {0.25f, 0.5f, 0.75f, 0.f};
    m.d3dMaterial.power = 24.f;
    m.diffuseColorSource = sc::TextureArgSource::VertexColor0;
    m.alphaTestEnabled = true;
    m.alphaTestCompareOp = sc::vk::COMPARE_OP_GREATER;
    m.alphaTestReferenceValue = 0x80;
    r = rr::legacyMaterial(m, true, false);
    check(approx(r.diffuse[1], 0.8f) && approx(r.diffuse[3], 0.5f), "D3DMATERIAL9 diffuse");
    check(approx(r.emissive[2], 0.75f), "D3DMATERIAL9 emissive");
    check((r.flags & rr::kMatVertexColor) != 0, "COLOR0 as the diffuse argument");
    check((r.flags & rr::kMatAlphaTest) != 0 && r.alphaTest == (sc::vk::COMPARE_OP_GREATER | 0x80u << 8),
          "alpha test op + reference");
    check(approx(r.roughness, std::sqrt(std::sqrt(2.f / 26.f)), 1e-5f), "power 24 -> GGX roughness");
    check((rr::legacyMaterial(m, false, false).flags & rr::kMatVertexColor) == 0, "no COLOR0 stream: material diffuse");
    check((rr::legacyMaterial(m, true, true).flags & rr::kMatUnlit) != 0, "sky: unlit");
    check(approx(rr::roughnessFromPower(0.f), 1.f), "power 0: rough");
    // Normal matrix of a non-uniform scale: inverse transpose.
    const float s[16] = {2, 0, 0, 0, 0, 4, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};
    float n[16];
    check(rr::normalMatrix(s, n) && approx(n[0], 0.5f) && approx(n[5], 0.25f) && approx(n[10], 1.f) && n[12] == 0.f,
          "normal matrix = inverse transpose (no translation)");
    const float a[16] = {1, 2, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 3, 0, 0, 1};
    float ab[16];
    rr::multiplyRowMajor(a, s, ab);
    check(approx(ab[0], 2.f) && approx(ab[1], 8.f) && approx(ab[12], 11.f) && approx(ab[13], 6.f), "row-vector product");
}

// ---- synthetic frame ------------------------------------------------------------------------------------------------

struct Mesh {
    std::vector<float> pos;          ///< xyz
    std::vector<std::uint16_t> idx;  ///< empty: non-indexed
};

geo::CapturedDrawPtr capturedDraw(const Mesh& m, std::uint32_t topology) {
    auto d = std::make_shared<geo::CapturedDraw>();
    d->status = geo::CaptureStatus::Captured;
    d->topology = topology;
    d->vertexCount = static_cast<std::uint32_t>(m.pos.size() / 3);
    auto bytes = std::make_shared<geo::ByteBuffer>(m.pos.size() * sizeof(float) + 16, std::uint8_t{0});
    std::memcpy(bytes->data(), m.pos.data(), m.pos.size() * sizeof(float));
    d->vertices.position.data = bytes;
    d->vertices.position.stride = 12;
    d->vertices.position.type = fuse::relight::hash::D3DDeclType::Float3;
    d->vertices.position.windowBytes = m.pos.size() * sizeof(float);
    if (!m.idx.empty()) {
        std::vector<std::uint8_t> ib(m.idx.size() * 2);
        std::memcpy(ib.data(), m.idx.data(), ib.size());
        d->indices = std::make_shared<geo::RebasedIndices>(2u, static_cast<std::uint32_t>(m.idx.size()), 0u,
                                                           d->vertexCount - 1u, std::move(ib));
        d->indexCount = static_cast<std::uint32_t>(m.idx.size());
    }
    return d;
}

/// A D3D LH look-at view (eye (0, 0, -5) looking at +z) and a perspective projection (fov 60, aspect 4:3, 0.1..100).
void camera(sc::DrawTransforms& t) {
    t.objectToWorld = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    t.worldToView = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 5, 1};
    const float yScale = 1.f / std::tan(0.5f * 1.04719755f), xScale = yScale / (4.f / 3.f);
    const float zn = 0.1f, zf = 100.f;
    t.viewToProjection = {xScale, 0, 0, 0, 0, yScale, 0, 0, 0, 0, zf / (zf - zn), 1, 0, 0, -zn * zf / (zf - zn), 0};
    t.textureTransform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
}

rt::CaptureDrawRecord record(const Mesh& m, std::uint32_t topology, std::uint32_t index) {
    rt::CaptureDrawRecord r;
    r.drawInFrame = index;
    r.translated = true;
    r.translation.translated = true;
    r.translation.cameraType = sc::CameraType::Main;
    r.translation.zEnable = true;
    r.translation.zWriteEnable = true;
    camera(r.translation.transforms);
    r.geometry = capturedDraw(m, topology);
    return r;
}

void testBuild() {
    const Mesh quad{{-1, -1, 0, -1, 1, 0, 1, 1, 0, 1, -1, 0}, {}};
    const Mesh indexed{{-1, -1, 0, -1, 1, 0, 1, 1, 0, 1, -1, 0}, {0, 1, 2, 0, 2, 3}};
    std::vector<rt::CaptureDrawRecord> draws;
    draws.push_back(record(quad, 5, 0));    // 0 fan: sky
    draws.push_back(record(indexed, 3, 1)); // 1 indexed list: opaque, fogged
    draws.push_back(record(quad, 5, 2));    // 2 fan: blended
    draws.push_back(record(quad, 4, 3));    // 3 strip: opaque
    draws.push_back(record(quad, 5, 4));    // 4 fan: decal
    draws.push_back(record(quad, 1, 5));    // 5 point list: skipped
    draws.push_back(record(quad, 5, 6));    // 6 fan: blended, after 2
    draws.push_back(record(quad, 5, 7));    // 7 not a scene draw
    draws[1].translation.fog.mode = sc::d3dff::FOG_LINEAR;
    draws[1].translation.fog.end = 50.f;
    draws[1].translation.fog.scale = 1.f / 40.f;
    draws[2].translation.material.blendMode.enableBlending = true;
    draws[6].translation.material.blendMode.enableBlending = true;
    draws[1].translation.transforms.objectToWorld[12] = 3.f; // world x + 3
    std::vector<sc::DrawClassification> cls(draws.size());
    cls[0].categories.set(sc::InstanceCategories::Sky);
    cls[4].categories.set(sc::InstanceCategories::DecalStatic);
    std::vector<std::uint8_t> sceneDraw(draws.size(), 1);
    sceneDraw[7] = 0;

    rr::BuildInputs in;
    in.frame = 9;
    in.draws = &draws;
    in.count = static_cast<std::uint32_t>(draws.size());
    in.classifications = &cls;
    in.sceneDraw = &sceneDraw;
    in.width = 128;
    in.height = 96;
    in.haveClear = true;
    in.clearColor = 0xff102030u;
    // Lights: a directional one (slot 4), a point light right of the camera (slot 7), one left (slot 9), one in the
    // GPU scene's past (no slot: skipped).
    auto point = [](float x, float y, float z, std::uint32_t slot) {
        rr::RasterLight l;
        l.light.type = static_cast<std::uint32_t>(gs::GpuLightType::Point);
        l.light.position[0] = x;
        l.light.position[1] = y;
        l.light.position[2] = z;
        l.light.range = 1.5f;
        l.light.intensity = 1.f;
        l.slot = slot;
        return l;
    };
    rr::RasterLight sun;
    sun.light.type = static_cast<std::uint32_t>(gs::GpuLightType::Directional);
    sun.light.direction[1] = -1.f;
    sun.light.intensity = 2.f;
    sun.slot = 4;
    in.lights = {sun, point(2.5f, 0.f, 0.f, 7), point(-2.5f, 0.f, 0.f, 9), point(0.f, 0.f, 0.f, 0xffffffffu)};

    rr::BuildOptions opt;
    opt.features = rr::tierFeatures(1) & rr::implementedFeatures();
    rr::RasterFrame f;
    check(rr::buildRasterFrame(in, opt, f), "frame built");
    check(f.camera.valid && f.camera.perspective && approx(f.camera.eye[2], -5.f) && approx(f.camera.forward[2], 1.f) &&
              approx(f.camera.nearZ, 0.1f, 1e-3f) && approx(f.camera.farZ, 100.f, 0.05f) &&
              approx(f.camera.fovY, 1.04719755f, 1e-4f),
          "main camera from the D3D view / projection");
    check(f.stats.opaque == 3 && f.stats.blended == 2 && f.stats.decals == 1 && f.stats.skipped == 1 &&
              f.stats.unlit == 1 && f.stats.draws == 7,
          "buckets: 3 opaque (sky, list, strip), 2 blended, 1 decal, 1 skipped");
    check(f.draws.size() == 7 && f.draws[5].bucket == rr::Bucket::Skipped &&
              std::string(f.draws[5].skipReason) == "not_triangles",
          "point lists skipped");
    check(f.draws[1].vertexCount == 6 && f.draws[0].vertexCount == 6 && f.draws[3].vertexCount == 6,
          "fans / strips / indexed lists de-indexed into 2 triangles");
    // Strip (0 1 2 3): triangles (0 1 2) and (2 1 3) - D3D's alternating winding.
    const rr::RasterDraw& strip = f.draws[3];
    const rr::RasterVertex* sv = &f.vertices[strip.firstVertex];
    check(approx(sv[3].pos[0], 1.f) && approx(sv[3].pos[1], 1.f) && approx(sv[4].pos[1], 1.f) && approx(sv[4].pos[0], -1.f) &&
              approx(sv[5].pos[0], 1.f) && approx(sv[5].pos[1], -1.f),
          "strip odd triangle keeps D3D's winding");
    std::vector<std::uint32_t> blendOrder;
    for (const rr::RasterDraw& d : f.draws) {
        if (d.bucket == rr::Bucket::Blend) {
            blendOrder.push_back(d.source);
        }
    }
    check(blendOrder == std::vector<std::uint32_t>{2, 6}, "blended draws keep submission order");
    check((f.gpuDraws[f.draws[0].gpu].material.flags & rr::kMatUnlit) != 0, "sky draw unlit");
    check((f.gpuDraws[f.draws[1].gpu].material.flags & rr::kMatFog) != 0 && f.stats.fogMode == sc::d3dff::FOG_LINEAR &&
              approx(f.constants.fogParams[1], 50.f),
          "frame fog from the first fogged draw");
    check((f.gpuDraws[f.draws[0].gpu].material.flags & rr::kMatFog) == 0, "sky not fogged");
    check(approx(f.gpuDraws[f.draws[1].gpu].objectToWorld[12], 3.f), "objectToWorld uploaded as is");
    check(approx(f.constants.clearColor[2], 0x30 / 255.f) && approx(f.constants.clearColor[0], 0x10 / 255.f),
          "clear colour");
    // Lights.
    check(f.directional == std::vector<std::uint32_t>{4}, "directional list = GPU-scene slots");
    check(f.stats.lights == 3 && f.stats.clustered == 2, "lights without a GPU-scene slot are skipped");
    check(!f.stats.fallbackLight && f.constants.fallbackDir[3] == 0.f, "no fallback with game lights");
    std::uint32_t rightTiles = 0, leftTiles = 0, other = 0;
    const std::uint32_t tx = 16, ty = 9, tz = 16;
    for (std::uint32_t c = 0; c < tx * ty * tz; ++c) {
        const std::uint32_t tileX = (c / tz) % tx;
        for (std::uint32_t k = 0; k < f.grid[2 * c + 1]; ++k) {
            const std::uint32_t slot = f.lightList[f.grid[2 * c] + k];
            if (slot == 7) {
                rightTiles += tileX >= tx / 2 ? 1u : 0u;
                other += tileX < tx / 2 - 1 ? 1u : 0u;
            } else if (slot == 9) {
                leftTiles += tileX < tx / 2 ? 1u : 0u;
                other += tileX > tx / 2 ? 1u : 0u;
            } else {
                ++other;
            }
        }
    }
    check(rightTiles > 0 && leftTiles > 0 && other == 0,
          "clustered: the light at +x lands in the right half, -x in the left (" + std::to_string(rightTiles) + "/" +
              std::to_string(leftTiles) + "/" + std::to_string(other) + ")");
    // Shadows (T1): the casters (opaque, not sky) inside the light's clip volume.
    check(f.shadow && f.shadowLight == 4, "shadow camera for the strongest distant light");
    for (const rr::RasterDraw& d : f.draws) {
        if (!d.castShadow) {
            continue;
        }
        const rr::RasterDrawGpu& g = f.gpuDraws[d.gpu];
        for (std::uint32_t v = d.firstVertex; v < d.firstVertex + d.vertexCount; ++v) {
            float w[4], c[4];
            const float p[4] = {f.vertices[v].pos[0], f.vertices[v].pos[1], f.vertices[v].pos[2], 1.f};
            for (int j = 0; j < 4; ++j) {
                w[j] = p[0] * g.objectToWorld[j] + p[1] * g.objectToWorld[4 + j] + p[2] * g.objectToWorld[8 + j] +
                       g.objectToWorld[12 + j];
            }
            for (int j = 0; j < 4; ++j) {
                c[j] = w[0] * f.shadowMatrix[j] + w[1] * f.shadowMatrix[4 + j] + w[2] * f.shadowMatrix[8 + j] +
                       w[3] * f.shadowMatrix[12 + j];
            }
            check(std::fabs(c[0]) <= 1.f && std::fabs(c[1]) <= 1.f && c[2] >= 0.f && c[2] <= 1.f,
                  "caster vertex inside the shadow clip volume");
        }
    }
    // T0: no shadows; no lights: the fallback light.
    opt.features = rr::tierFeatures(0);
    in.lights.clear();
    check(rr::buildRasterFrame(in, opt, f), "T0 frame");
    check(!f.shadow && (f.constants.counts[3] & rr::kFeatureShadows) == 0, "T0: no shadow map");
    check(f.stats.fallbackLight && f.constants.fallbackDir[3] == 1.f && approx(f.constants.fallbackColor[1], 1.8f),
          "no light: rtx.fallbackLight* distant light");
    opt.fallbackLightMode = 0;
    check(rr::buildRasterFrame(in, opt, f) && !f.stats.fallbackLight, "rtx.fallbackLightMode = 0");
    // No scene draw: nothing to render.
    sceneDraw.assign(draws.size(), 0);
    check(!rr::buildRasterFrame(in, opt, f), "no scene draw -> passthrough");
}

} // namespace

int main(int argc, char** argv) {
    const std::string suite = argc > 1 ? argv[1] : "all";
    if (suite == "tiers" || suite == "all") {
        testTiers();
    }
    if (suite == "material" || suite == "all") {
        testMaterial();
    }
    if (suite == "build" || suite == "all") {
        testBuild();
    }
    if (g_failures) {
        std::printf("FAIL: %d check(s) (%s)\n", g_failures, suite.c_str());
        return 1;
    }
    std::printf("PASS: rl_raster_cpu %s\n", suite.c_str());
    return 0;
}
