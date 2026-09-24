// FUSE Relight RL-4.2: CPU gates of the raster remaster's frame builder (raster_scene.hpp), no Vulkan.
//
//   tiers     planTier: the T0 / T1 / T2 feature masks, the device / option / FUSE_RENDER_TIER_MAX caps and graceful
//             degradation (features this build lacks, down to G-buffer + clustered);
//   material  the legacy material mapping (D3D material colours, the never-set default, COLOR0 as diffuse, alpha
//             test, specular power -> roughness, sky unlit) and the matrix helpers (inverse-transpose normals);
//   build     a synthetic frame: buckets (opaque / sky / decal / blend / skipped), de-indexing of lists, strips (D3D
//             winding) and fans, the blend order (submission), fog selection, the directional list, the clustered
//             light assignment (a light right of the camera lands in the right half of the grid: the mirrored
//             handedness fix), the fallback light, the shadow camera (casters inside its clip volume);
//   viewport  per-draw D3D viewport rectangles (RL-1.5's FUSE addition): carried per draw, clamped to the target, the
//             whole target without one, counted; the main camera's VIEW x PROJECTION in the frame constants;
//   positions programmable-VS draws from the RL-1.6 capture (object-space positions when the D3D transforms reproduce
//             the captured clip positions, else the clip positions through the main camera's inverse VIEW x
//             PROJECTION: both re-project to the captured NDC), VS output texcoord / colour, the capture slot of
//             an indexed window vertex, missing captures skipped; POSITIONT (XYZRHW) raster-only draws through
//             DXVK's inverse viewport mapping (their re-projection lands on the D3D pixel + 0.5 and keeps z), unlit,
//             no shadow casting, the stencil test skipped;
//   emissive  D3DRS_EMISSIVEMATERIALSOURCE: COLOR0 / COLOR1 emissive flags, COLOR1 in the vertex, replacement
//             materials override it.
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

// ---- viewport / positions / emissive (RL-4.2 open issues) ------------------------------------------------------------

/// A draw record whose geometry is POSITIONT (FLOAT4 x, y, z, rhw) quads.
rt::CaptureDrawRecord preTransformedRecord(const std::vector<float>& xyzrhw, std::uint32_t index) {
    rt::CaptureDrawRecord r;
    r.drawInFrame = index;
    r.translated = true;
    r.translation.rasterOnly = true;
    r.translation.zEnable = true;
    r.translation.zWriteEnable = true;
    r.translation.transforms.textureTransform = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    auto d = std::make_shared<geo::CapturedDraw>();
    d->status = geo::CaptureStatus::Captured;
    d->topology = 5; // fan
    d->vertexCount = static_cast<std::uint32_t>(xyzrhw.size() / 4);
    auto bytes = std::make_shared<geo::ByteBuffer>(xyzrhw.size() * sizeof(float) + 16, std::uint8_t{0});
    std::memcpy(bytes->data(), xyzrhw.data(), xyzrhw.size() * sizeof(float));
    d->vertices.position.data = bytes;
    d->vertices.position.stride = 16;
    d->vertices.position.type = fuse::relight::hash::D3DDeclType::Float4;
    d->vertices.position.windowBytes = xyzrhw.size() * sizeof(float);
    d->vertices.hasPositionT = true;
    r.geometry = d;
    return r;
}

/// p (w = 1) x m, D3D row-vector convention.
void project(const float* m, const float* p, float* out) {
    for (int c = 0; c < 4; ++c) {
        out[c] = p[0] * m[c] + p[1] * m[4 + c] + p[2] * m[8 + c] + m[12 + c];
    }
}

struct Frame {
    std::vector<rt::CaptureDrawRecord> draws;
    std::vector<sc::DrawClassification> cls;
    std::vector<std::uint8_t> scene;
    rr::BuildInputs in;
    rr::BuildOptions opt;
    rr::RasterFrame f;
    bool build() {
        cls.assign(draws.size(), sc::DrawClassification{});
        scene.assign(draws.size(), 1);
        in.draws = &draws;
        in.count = static_cast<std::uint32_t>(draws.size());
        in.classifications = &cls;
        in.sceneDraw = &scene;
        in.width = 128;
        in.height = 96;
        opt.features = rr::tierFeatures(1) & rr::implementedFeatures();
        return rr::buildRasterFrame(in, opt, f);
    }
};

void testViewport() {
    const Mesh quad{{-1, -1, 0, -1, 1, 0, 1, 1, 0, 1, -1, 0}, {}};
    Frame fr;
    fr.draws.push_back(record(quad, 5, 0));
    for (int q = 0; q < 4; ++q) {
        rt::CaptureDrawRecord r = record(quad, 5, static_cast<std::uint32_t>(1 + q));
        r.translation.viewportX = static_cast<std::uint32_t>(q % 2) * 64u;
        r.translation.viewportY = static_cast<std::uint32_t>(q / 2) * 48u;
        r.translation.viewportWidth = 64;
        r.translation.viewportHeight = 48;
        fr.draws.push_back(r);
    }
    rt::CaptureDrawRecord big = record(quad, 5, 5);
    big.translation.viewportX = 100;
    big.translation.viewportY = 10;
    big.translation.viewportWidth = 64; // past the target's right edge
    big.translation.viewportHeight = 200;
    fr.draws.push_back(big);
    rt::CaptureDrawRecord full = record(quad, 5, 6);
    full.translation.viewportWidth = 128;
    full.translation.viewportHeight = 96;
    fr.draws.push_back(full);
    check(fr.build(), "viewport frame built");
    const rr::RasterFrame& f = fr.f;
    check(f.draws[0].viewportWidth == 0, "no rectangle: the whole target");
    bool quadrants = true;
    for (int q = 0; q < 4; ++q) {
        const rr::RasterDraw& d = f.draws[static_cast<std::size_t>(1 + q)];
        quadrants = quadrants && d.viewportX == static_cast<std::uint32_t>(q % 2) * 64u &&
                    d.viewportY == static_cast<std::uint32_t>(q / 2) * 48u && d.viewportWidth == 64 &&
                    d.viewportHeight == 48;
    }
    check(quadrants, "four quadrant viewports carried per draw (disjoint rectangles)");
    check(f.draws[5].viewportX == 100 && f.draws[5].viewportWidth == 28 && f.draws[5].viewportHeight == 86,
          "a rectangle past the target is clamped to it");
    check(f.stats.viewports == 5, "5 draws with a rectangle smaller than the target (" +
                                      std::to_string(f.stats.viewports) + ")");
    float vp[16];
    rr::multiplyRowMajor(fr.draws[0].translation.transforms.worldToView.data(),
                         fr.draws[0].translation.transforms.viewToProjection.data(), vp);
    bool same = true;
    for (int i = 0; i < 16; ++i) {
        same = same && f.constants.viewProj[i] == vp[i];
    }
    check(same, "frame constants carry the main camera's VIEW x PROJECTION (cluster lookup)");
    float inv[16], id[16];
    check(rr::invertMatrix(vp, inv), "VIEW x PROJECTION invertible");
    rr::multiplyRowMajor(vp, inv, id);
    bool ident = true;
    for (int i = 0; i < 16; ++i) {
        ident = ident && approx(id[i], (i % 5 == 0) ? 1.f : 0.f, 1e-4f);
    }
    check(ident, "invertMatrix x matrix = identity");
    const float singular[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    check(!rr::invertMatrix(singular, inv), "singular matrix rejected");
}

void testPreTransformed() {
    // DXVK's mapping on its own: pixel (x, y) of a 64 x 48 viewport at (64, 48), rhw 0.5 -> clip w 2.
    float clip[4];
    const float v[4] = {96.f, 72.f, 0.25f, 0.5f};
    rr::preTransformedToClip(v, 64.f, 48.f, 64.f, 48.f, true, clip);
    check(approx(clip[3], 2.f) && approx(clip[0] / clip[3], 0.f) && approx(clip[1] / clip[3], 0.f) &&
              approx(clip[2] / clip[3], 0.25f),
          "POSITIONT -> clip: viewport centre, z kept, w = 1 / rhw");
    rr::preTransformedToClip(v, 64.f, 48.f, 64.f, 48.f, false, clip);
    check(clip[2] == 0.f, "POSITIONT without a depth test: z 0 (DXVK)");
    const float zeroRhw[4] = {64.f, 48.f, 0.5f, 0.f};
    rr::preTransformedToClip(zeroRhw, 64.f, 48.f, 64.f, 48.f, true, clip);
    check(clip[3] == 1.f && approx(clip[0], -1.f) && approx(clip[1], 1.f), "rhw 0: w 1 (DXVK); top-left corner");

    // A frame: a camera draw, a POSITIONT quad in the right half (rhw 0.25, z 0.9), one with the stencil test on.
    const Mesh quad{{-1, -1, 0, -1, 1, 0, 1, 1, 0, 1, -1, 0}, {}};
    Frame fr;
    fr.draws.push_back(record(quad, 5, 0));
    const std::vector<float> rhw = {70, 10, 0.9f, 0.25f, 120, 10, 0.9f, 0.25f, 120, 80, 0.9f, 0.25f, 70, 80, 0.9f, 0.25f};
    fr.draws.push_back(preTransformedRecord(rhw, 1));
    fr.draws.back().translation.viewportWidth = 128;
    fr.draws.back().translation.viewportHeight = 96;
    fr.draws.push_back(preTransformedRecord(rhw, 2));
    fr.draws.back().translation.stencilEnabled = true;
    rt::CaptureDrawRecord ui = preTransformedRecord(rhw, 3); // Remix's UI / not translated: never a scene draw
    ui.translation.rasterOnly = false;
    fr.draws.push_back(ui);
    check(fr.build(), "POSITIONT frame built");
    const rr::RasterFrame& f = fr.f;
    const rr::RasterDraw& d = f.draws[1];
    check(d.bucket == rr::Bucket::Opaque && d.positions == rr::PositionSource::PreTransformed && d.vertexCount == 6,
          "raster-only POSITIONT quad drawn (2 triangles)");
    check(f.stats.preTransformed == 1, "one pre-transformed draw counted");
    const rr::RasterDrawGpu& g = f.gpuDraws[d.gpu];
    check((g.material.flags & rr::kMatUnlit) != 0 && !d.castShadow, "POSITIONT: unlit, casts no shadow");
    bool pixels = true;
    for (std::uint32_t k = 0; k < d.vertexCount; ++k) {
        const rr::RasterVertex& rv = f.vertices[d.firstVertex + k];
        float c[4];
        project(g.worldToClip, rv.pos, c);
        // Window coordinates as DXVK's viewport maps them (x + 0.5, y + 0.5 for a full-target viewport).
        const float wx = (c[0] / c[3] * 0.5f + 0.5f) * 128.f + 0.5f, wy = (0.5f - c[1] / c[3] * 0.5f) * 96.f + 0.5f;
        const bool matches = (approx(wx, 70.5f, 2e-3f) || approx(wx, 120.5f, 2e-3f)) &&
                             (approx(wy, 10.5f, 2e-3f) || approx(wy, 80.5f, 2e-3f)) && approx(c[2] / c[3], 0.9f, 1e-4f) &&
                             c[3] > 0.f;
        pixels = pixels && matches;
    }
    check(pixels, "the main camera re-projects the mapped world positions onto the D3D pixels (+ 0.5) at depth z");
    check(f.draws[2].bucket == rr::Bucket::Skipped && std::string(f.draws[2].skipReason) == "stencil",
          "POSITIONT with the stencil test: skipped");
    check(f.draws[3].bucket == rr::Bucket::Skipped && std::string(f.draws[3].skipReason) == "not_translated",
          "untranslated POSITIONT (UI): skipped");
}

/// A programmable-VS draw: indexed quad (indices 10..13, BaseVertexIndex 5) with its capture region.
rt::CaptureDrawRecord vsRecord(const float* wvp, bool consistent, bool writeColor) {
    const Mesh quad{{-1, -1, 0, -1, 1, 0, 1, 1, 0, 1, -1, 0}, {0, 1, 2, 0, 2, 3}};
    rt::CaptureDrawRecord r = record(quad, 3, 1);
    r.translation.cameraType = sc::CameraType::Unknown;
    auto g = std::const_pointer_cast<geo::CapturedDraw>(r.geometry);
    g->programmableVs = true;
    // The rebased indices of the app's 10..13 (minIndex 10), window at BaseVertexIndex 5 + 10.
    std::vector<std::uint8_t> ib(12);
    const std::uint16_t rebased[6] = {0, 1, 2, 0, 2, 3};
    std::memcpy(ib.data(), rebased, sizeof(rebased));
    g->indices = std::make_shared<geo::RebasedIndices>(2u, 6u, 10u, 13u, std::move(ib));
    g->vertexIndexOffset = 15;
    auto cap = std::make_shared<fuse::relight::capture::vertex_capture::DrawVertexCapture>();
    cap->captured = true;
    cap->baseVertex = 13; // DrawIndexedPrimitive(BaseVertexIndex 5, MinVertexIndex 8): slot k <-> index 8 + k
    cap->vertexOffset = 5;
    cap->raw.resize(8);
    cap->vertices.resize(8);
    const float obj[4][3] = {{-1, -1, 0}, {-1, 1, 0}, {1, 1, 0}, {1, -1, 0}};
    for (int k = 0; k < 4; ++k) {
        const std::size_t slot = static_cast<std::size_t>(2 + k); // index 10 + k
        auto& raw = cap->raw[slot];
        const float p[3] = {obj[k][0] * 0.5f + 0.25f, obj[k][1] * 0.5f, obj[k][2]};
        project(wvp, p, raw.clip);
        raw.fields = fuse::relight::capture::vertex_capture::fields::kWritten |
                     fuse::relight::capture::vertex_capture::fields::kTexcoord |
                     (writeColor ? fuse::relight::capture::vertex_capture::fields::kColor : 0u);
        auto& cv = cap->vertices[slot];
        // Consistent: the back-transform recovers p. Inconsistent: object positions that do not re-project (as when the
        // shader ignores D3DTS_*).
        cv.position[0] = consistent ? p[0] : p[0] * 7.f;
        cv.position[1] = consistent ? p[1] : p[1] * 7.f;
        cv.position[2] = consistent ? p[2] : 3.f;
        cv.texcoord0[0] = 0.125f * static_cast<float>(k);
        cv.texcoord0[1] = 0.5f;
        cv.color0 = 0x80ff4020u; // ARGB
    }
    cap->written = 4;
    r.vertexCapture = cap;
    return r;
}

void testVertexCapture() {
    const Mesh quad{{-1, -1, 0, -1, 1, 0, 1, 1, 0, 1, -1, 0}, {}};
    Frame fr;
    fr.draws.push_back(record(quad, 5, 0));
    float wvp[16];
    rr::multiplyRowMajor(fr.draws[0].translation.transforms.worldToView.data(),
                         fr.draws[0].translation.transforms.viewToProjection.data(), wvp);
    fr.draws.push_back(vsRecord(wvp, true, true));
    fr.draws.push_back(vsRecord(wvp, false, false));
    rt::CaptureDrawRecord missing = vsRecord(wvp, true, true);
    missing.vertexCapture->captured = false;
    fr.draws.push_back(missing);
    rt::CaptureDrawRecord off = vsRecord(wvp, true, true);
    off.vertexCapture.reset();
    fr.draws.push_back(off);
    check(fr.build(), "vertex-capture frame built");
    const rr::RasterFrame& f = fr.f;
    auto reprojects = [&](const rr::RasterDraw& d, const rt::CaptureDrawRecord& r) {
        const rr::RasterDrawGpu& g = f.gpuDraws[d.gpu];
        const std::uint16_t order[6] = {0, 1, 2, 0, 2, 3};
        bool ok = d.vertexCount == 6;
        for (std::uint32_t k = 0; ok && k < 6; ++k) {
            const rr::RasterVertex& rv = f.vertices[d.firstVertex + k];
            float w[4], c[4];
            project(g.objectToWorld, rv.pos, w);
            const float w3[3] = {w[0] / w[3], w[1] / w[3], w[2] / w[3]};
            project(g.worldToClip, w3, c);
            const auto& raw = r.vertexCapture->raw[static_cast<std::size_t>(2 + order[k])];
            ok = ok && approx(c[0] / c[3], raw.clip[0] / raw.clip[3], 1e-4f) &&
                 approx(c[1] / c[3], raw.clip[1] / raw.clip[3], 1e-4f) &&
                 approx(c[2] / c[3], raw.clip[2] / raw.clip[3], 1e-4f) &&
                 approx(rv.uv[0], 0.125f * static_cast<float>(order[k]));
        }
        return ok;
    };
    check(f.draws[1].positions == rr::PositionSource::VertexCapture && reprojects(f.draws[1], fr.draws[1]),
          "consistent VS draw: RL-1.6 object positions (slot of index 10 + k), re-projecting to the captured NDC");
    check(f.vertices[f.draws[1].firstVertex].color == (0xffu | 0x40u << 8 | 0x20u << 16 | 0x80u << 24),
          "captured COLOR0 (D3DCOLOR 0x80ff4020) -> RGBA8");
    check((f.gpuDraws[f.draws[1].gpu].material.flags & rr::kMatVertexColor) != 0 &&
              (f.gpuDraws[f.draws[1].gpu].material.flags & rr::kMatHasNormals) == 0,
          "VS COLOR0 output is the diffuse; flat normals");
    check(f.draws[2].positions == rr::PositionSource::CaptureClip && reprojects(f.draws[2], fr.draws[2]),
          "inconsistent VS draw: captured clip positions through the inverse main camera");
    check((f.gpuDraws[f.draws[2].gpu].material.flags & rr::kMatVertexColor) == 0, "no COLOR0 output: material diffuse");
    check(f.draws[3].bucket == rr::Bucket::Skipped && std::string(f.draws[3].skipReason) == "vertex_capture_missing",
          "region not delivered: skipped");
    check(f.draws[4].bucket == rr::Bucket::Skipped && std::string(f.draws[4].skipReason) == "programmable_vs",
          "vertex capture off: skipped");
    check(f.stats.vertexCaptured == 1 && f.stats.captureClip == 1, "stats: 1 object-space, 1 clip-space VS draw");
}

void testEmissive() {
    sc::LegacyMaterialRecord m;
    m.d3dMaterial.diffuse = {0.5f, 0.5f, 0.5f, 1.f};
    m.emissiveSource = sc::EmissiveSource::VertexColor0;
    check((rr::legacyMaterial(m, true, false).flags & rr::kMatEmissiveColor0) != 0, "emissive = COLOR0");
    check((rr::legacyMaterial(m, false, false).flags & (rr::kMatEmissiveColor0 | rr::kMatEmissiveColor1)) == 0,
          "emissive COLOR0 without COLOR0 in the vertex: material");
    m.emissiveSource = sc::EmissiveSource::VertexColor1;
    check((rr::legacyMaterial(m, true, false, true).flags & rr::kMatEmissiveColor1) != 0, "emissive = COLOR1");
    check((rr::legacyMaterial(m, true, false, false).flags & rr::kMatEmissiveColor1) == 0,
          "emissive COLOR1 without COLOR1: material");
    // In a frame: COLOR0 / COLOR1 streams, the COLOR1 value in the vertex.
    const Mesh quad{{-1, -1, 0, -1, 1, 0, 1, 1, 0, 1, -1, 0}, {}};
    Frame fr;
    fr.draws.push_back(record(quad, 5, 0));
    rt::CaptureDrawRecord r = record(quad, 5, 1);
    auto g = std::const_pointer_cast<geo::CapturedDraw>(r.geometry);
    auto colors = std::make_shared<geo::ByteBuffer>(4 * 8 + 8, std::uint8_t{0});
    for (int k = 0; k < 4; ++k) {
        const std::uint32_t c0 = 0xff102030u, c1 = 0xff405060u; // D3DCOLOR ARGB (bytes B G R A)
        std::memcpy(colors->data() + 8 * k, &c0, 4);
        std::memcpy(colors->data() + 8 * k + 4, &c1, 4);
    }
    for (geo::VertexAttribute* a : {&g->vertices.color0, &g->vertices.color1}) {
        a->data = colors;
        a->stride = 8;
        a->type = fuse::relight::hash::D3DDeclType::D3DColor;
        a->windowBytes = 32;
    }
    g->vertices.color1.offset = 4;
    r.translation.material.emissiveSource = sc::EmissiveSource::VertexColor1;
    fr.draws.push_back(r);
    check(fr.build(), "emissive frame built");
    const rr::RasterDraw& d = fr.f.draws[1];
    const rr::RasterVertex& rv = fr.f.vertices[d.firstVertex];
    check((fr.f.gpuDraws[d.gpu].material.flags & rr::kMatEmissiveColor1) != 0 && fr.f.stats.emissiveVertex == 1,
          "COLOR1 emissive draw");
    check(rv.color == (0x10u | 0x20u << 8 | 0x30u << 16 | 0xffu << 24) &&
              rv.color1 == (0x40u | 0x50u << 8 | 0x60u << 16 | 0xffu << 24),
          "COLOR0 / COLOR1 in the vertex as RGBA8");
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
    if (suite == "viewport" || suite == "all") {
        testViewport();
    }
    if (suite == "positions" || suite == "all") {
        testPreTransformed();
        testVertexCapture();
    }
    if (suite == "emissive" || suite == "all") {
        testEmissive();
    }
    if (g_failures) {
        std::printf("FAIL: %d check(s) (%s)\n", g_failures, suite.c_str());
        return 1;
    }
    std::printf("PASS: rl_raster_cpu %s\n", suite.c_str());
    return 0;
}
