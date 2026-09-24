// FUSE Relight RL-1.2: unit tests of the draw classifier (ctest rl_classify_unit).
//
// One test per rule, on synthetic D3DStateModels, each citing the Remix code it reproduces
// (dxvk-remix @0867d3c): d3d9_rtx.cpp makeDrawCallType / isRenderingUI / internalPrepareDraw /
// processTextures, d3d9_rtx_utils.cpp isRenderTargetPrimary, rtx_types.cpp setupCategoriesFor*,
// checkSkyAutoDetect, shouldBakeSky, shouldBakeTerrain, rtx_options.cpp decal migration,
// rtx_resources.cpp getFormatCompatibilityCategory. Options are set through an RL-0.6 layer built
// from rtx.conf text, as a game's rtx.conf would set them.
#include <fuse/relight/scene/classify/classify_options.hpp>
#include <fuse/relight/scene/classify/classify_tap.hpp>
#include <fuse/relight/scene/classify/draw_classifier.hpp>

#include <fuse/relight/hash/texture_hash.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <cstdio>
#include <cstring>
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

using namespace fuse::relight;
using namespace fuse::relight::scene;

/// The terrain baker owns rtx.terrainBaker.enableBaking; the test binary stands in for it.
struct TerrainBakerStandIn {
    FUSE_RELIGHT_OPTION("rtx.terrainBaker", bool, enableBaking, true, "Test stand-in for the terrain baker's option.");
};

/// rtx.conf text applied as an option layer for the scope's lifetime.
class ScopedConf {
public:
    explicit ScopedConf(const std::string& text) {
        static int s_counter = 0;
        const options::OptionConfig config = options::OptionConfig::parse(text);
        m_layer = options::OptionManager::acquireLayer("", {5000u + static_cast<std::uint32_t>(s_counter++), "rl_classify_test"},
                                                       1.0f, 0.1f, false, &config);
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

std::string hex(Hash64 h) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%016llX", static_cast<unsigned long long>(h));
    return buf;
}

constexpr std::uint32_t kBbW = 1280, kBbH = 720;
constexpr std::uint32_t kFmtA8R8G8B8 = 21, kFmtX8R8G8B8 = 22, kFmtNull = d3d::fourcc('N', 'U', 'L', 'L');

TextureRecord texture2d(tap::ResourceId id, Hash64 hash, std::uint32_t w = 64, std::uint32_t h = 64) {
    TextureRecord t;
    t.id = id;
    t.type = d3d::RTYPE_TEXTURE;
    t.width = w;
    t.height = h;
    t.format = kFmtA8R8G8B8;
    t.pool = 1;
    t.imageHash = hash;
    return t;
}

TextureRecord renderTarget(tap::ResourceId id, std::uint32_t w, std::uint32_t h, std::uint32_t format, bool texture) {
    TextureRecord t;
    t.id = id;
    t.type = texture ? d3d::RTYPE_TEXTURE : d3d::RTYPE_SURFACE;
    t.width = w;
    t.height = h;
    t.format = format;
    t.usage = d3d::USAGE_RENDERTARGET;
    t.imageHash = 0x5151000000000000ull + id;
    hash::TextureDescriptor desc;
    desc.width = w;
    desc.height = h;
    desc.usage = t.usage;
    desc.format = format;
    t.descriptorHash = hash::hashTextureDescriptor(desc);
    return t;
}

std::array<float, 16> perspective() {
    // perspectiveFovLH-like: [3][3] = 0, [2][3] = 1.
    return {1.3f, 0, 0, 0, 0, 1.7f, 0, 0, 0, 0, 1.0005f, 1, 0, 0, -0.1f, 0};
}
std::array<float, 16> ortho() { return {2.0f / 128, 0, 0, 0, 0, -2.0f / 96, 0, 0, 0, 0, 1, 0, -1, 1, 0, 1}; }
std::array<float, 16> viewAt(float x, float y, float z) {
    std::array<float, 16> v = identityMatrix();
    v[12] = x;
    v[13] = y;
    v[14] = z;
    return v;
}

/// A plain world draw: FF, textured with `hash` on stage 0, triangle list, primary back buffer.
D3DStateModel worldDraw(Hash64 hash = 0x1111) {
    D3DStateModel m;
    m.primitiveType = d3d::PT_TRIANGLELIST;
    m.primitiveCount = 12;
    m.indexed = true;
    m.backBufferWidth = kBbW;
    m.backBufferHeight = kBbH;
    m.renderTarget0 = renderTarget(1, kBbW, kBbH, kFmtX8R8G8B8, false);
    m.renderTarget0.isBackBuffer = true;
    m.projection = perspective();
    m.view = viewAt(0, -1, 5);
    if (hash != kEmptyHash) {
        m.textures[0] = texture2d(10, hash);
    }
    m.textureStages[0][d3d::TSS_COLOROP] = d3d::TOP_MODULATE;
    m.textureStages[0][d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
    m.textureStages[0][d3d::TSS_COLORARG2] = d3d::TA_DIFFUSE;
    return m;
}

DrawClassification classifyOnce(const D3DStateModel& m) {
    DrawClassifier c;
    return c.classify(m);
}

// ---- makeDrawCallType, in Remix order ------------------------------------------------------------------

// d3d9_rtx.cpp:412 - `m_drawCallID < drawCallRange().x || m_drawCallID > drawCallRange().y` after
// the post-increment: the first draw of a frame compares as 1.
void test_drawCallRange() {
    DrawClassifier c;
    CHECK(c.classify(worldDraw()).status == GeometryStatus::RayTraced); // default [0, INT32_MAX]
    ScopedConf conf("rtx.drawCallRange = 2, 3\n");
    DrawClassifier d;
    const auto r1 = d.classify(worldDraw());
    CHECK(r1.status == GeometryStatus::Ignored && r1.reason == ClassifyReason::DrawCallRange && r1.drawCallId == 0);
    CHECK(d.classify(worldDraw()).status == GeometryStatus::RayTraced);
    const auto r3 = d.classify(worldDraw());
    CHECK(r3.status == GeometryStatus::RayTraced && r3.drawCallId == 2);
    CHECK(d.classify(worldDraw()).reason == ClassifyReason::DrawCallRange);
    CHECK(r1.prepareFlags == prepare_draw::Ignore); // rtx.enableRaytracing: ignored draws are dropped
    d.endFrame();
    CHECK(d.classify(worldDraw()).reason == ClassifyReason::DrawCallRange); // counter restarts per frame
}

// d3d9_rtx.cpp:427 - programmable VS without rtx.useVertexCapture is ignored; with capture it is
// ray traced and the original draw is preserved (internalPrepareDraw preserveOriginalDraw).
void test_vertexShaderCapture() {
    D3DStateModel m = worldDraw();
    m.usesVertexShader = true;
    const auto r = classifyOnce(m);
    CHECK(r.status == GeometryStatus::RayTraced);
    CHECK(r.prepareFlags == (prepare_draw::CommitToRayTracing | prepare_draw::PreserveDrawCallAndItsState));
    CHECK(toTapDecision(r.prepareFlags) == tap::DrawDecision::RayTracedPreserveRaster);
    ScopedConf conf("rtx.useVertexCapture = False\n");
    CHECK(classifyOnce(m).reason == ClassifyReason::VertexShaderWithoutCapture);
    CHECK(classifyOnce(worldDraw()).status == GeometryStatus::RayTraced); // FF unaffected
}

// d3d9_rtx.cpp:432-442 - zero primitives and non-triangle topologies are ignored.
void test_primitives() {
    D3DStateModel m = worldDraw();
    m.primitiveCount = 0;
    CHECK(classifyOnce(m).reason == ClassifyReason::ZeroPrimitives);
    for (std::uint32_t pt : {d3d::PT_POINTLIST, d3d::PT_LINELIST, d3d::PT_LINESTRIP}) {
        m = worldDraw();
        m.primitiveType = pt;
        CHECK(classifyOnce(m).reason == ClassifyReason::UnsupportedTopology);
    }
    for (std::uint32_t pt : {d3d::PT_TRIANGLELIST, d3d::PT_TRIANGLESTRIP, d3d::PT_TRIANGLEFAN}) {
        m = worldDraw();
        m.primitiveType = pt;
        CHECK(classifyOnce(m).status == GeometryStatus::RayTraced);
    }
}

// d3d9_rtx.cpp:444-452 - rtx.enableAlphaTest / rtx.enableAlphaBlend off ignore such draws
// (IsAlphaTestEnabled excludes DXVK's alpha-to-coverage).
void test_alphaOptions() {
    D3DStateModel test = worldDraw();
    test.renderStates[d3d::RS_ALPHATESTENABLE] = 1;
    D3DStateModel blend = worldDraw();
    blend.renderStates[d3d::RS_ALPHABLENDENABLE] = 1;
    CHECK(classifyOnce(test).status == GeometryStatus::RayTraced);
    CHECK(classifyOnce(blend).status == GeometryStatus::RayTraced);
    {
        ScopedConf conf("rtx.enableAlphaTest = False\n");
        CHECK(classifyOnce(test).reason == ClassifyReason::AlphaTestDisabled);
        D3DStateModel atoc = test;
        atoc.renderStates[d3d::RS_ADAPTIVETESS_Y] = d3d::FMT_ATOC;
        atoc.renderTarget0.multiSample = 4;
        CHECK(!atoc.isAlphaTestEnabled());
        CHECK(classifyOnce(atoc).status == GeometryStatus::RayTraced);
        atoc.d3d8 = true; // DXVK: no alpha to coverage for D3D8
        CHECK(classifyOnce(atoc).reason == ClassifyReason::AlphaTestDisabled);
    }
    ScopedConf conf("rtx.enableAlphaBlend = False\n");
    CHECK(classifyOnce(blend).reason == ClassifyReason::AlphaBlendDisabled);
}

// d3d9_rtx.cpp:454 - an active occlusion query rasterizes the draw (no injection).
void test_occlusionQuery() {
    D3DStateModel m = worldDraw();
    m.activeOcclusionQueries = 1;
    const auto r = classifyOnce(m);
    CHECK(r.status == GeometryStatus::Rasterized && r.reason == ClassifyReason::OcclusionQuery && !r.triggerRtxInjection);
    CHECK(toTapDecision(r.prepareFlags) == tap::DrawDecision::Raster);
}

// d3d9_rtx.cpp:459-475 - no colour RT, an RT without GPU image, or RGB writes masked: ignored.
void test_colorTarget() {
    D3DStateModel m = worldDraw();
    m.renderTarget0 = TextureRecord{};
    CHECK(classifyOnce(m).reason == ClassifyReason::NoColorTarget);
    m = worldDraw();
    m.renderTarget0.hasImage = false;
    CHECK(classifyOnce(m).reason == ClassifyReason::TargetWithoutImage);
    m = worldDraw();
    m.renderStates[d3d::RS_COLORWRITEENABLE] = 0x7; // alpha off only: still ray traced
    CHECK(classifyOnce(m).status == GeometryStatus::RayTraced);
    for (std::uint32_t mask : {0x0u, 0x8u, 0xeu, 0xdu, 0xbu}) {
        m.renderStates[d3d::RS_COLORWRITEENABLE] = mask;
        CHECK(classifyOnce(m).reason == ClassifyReason::ColorWriteDisabled);
    }
}

// d3d9_rtx.cpp:481-491 - shadow mask: stage 0 SELECTARG1/2 of a non-texture argument, square RT
// narrower than a quarter of the back buffer, RT format without a compatibility class.
void test_shadowMask() {
    D3DStateModel m = worldDraw(kEmptyHash);
    m.textureStages[0][d3d::TSS_COLOROP] = d3d::TOP_SELECTARG1;
    m.textureStages[0][d3d::TSS_COLORARG1] = d3d::TA_DIFFUSE;
    m.renderTarget0 = renderTarget(2, 256, 256, kFmtNull, false);
    CHECK(isShadowMaskDraw(m));
    CHECK(classifyOnce(m).reason == ClassifyReason::ShadowMask);
    D3DStateModel arg2 = m;
    arg2.textureStages[0][d3d::TSS_COLOROP] = d3d::TOP_SELECTARG2;
    arg2.textureStages[0][d3d::TSS_COLORARG2] = d3d::TA_TFACTOR;
    CHECK(isShadowMaskDraw(arg2));
    D3DStateModel textured = m;
    textured.textureStages[0][d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
    CHECK(!isShadowMaskDraw(textured));
    D3DStateModel modulate = m;
    modulate.textureStages[0][d3d::TSS_COLOROP] = d3d::TOP_MODULATE;
    CHECK(!isShadowMaskDraw(modulate));
    D3DStateModel notSquare = m;
    notSquare.renderTarget0.height = 128;
    CHECK(!isShadowMaskDraw(notSquare));
    D3DStateModel tooBig = m;
    tooBig.renderTarget0.width = tooBig.renderTarget0.height = kBbW / 4; // not strictly smaller
    CHECK(!isShadowMaskDraw(tooBig));
    D3DStateModel colorFormat = m;
    colorFormat.renderTarget0.format = kFmtA8R8G8B8; // B8G8R8A8: 32-bit class
    CHECK(!isShadowMaskDraw(colorFormat));
    CHECK(classifyOnce(colorFormat).reason == ClassifyReason::NonPrimaryTarget);
}

// d3d9_rtx.cpp:493-512, 419-426, 575-601, 750 - raytraced render targets (by descriptor hash).
void test_raytracedRenderTarget() {
    const TextureRecord rt = renderTarget(3, 256, 256, kFmtA8R8G8B8, true);
    D3DStateModel into = worldDraw();
    into.renderTarget0 = rt;
    CHECK(classifyOnce(into).reason == ClassifyReason::NonPrimaryTarget); // not listed: rasterized
    D3DStateModel sampling = worldDraw();
    sampling.textures[0] = rt;
    {
        ScopedConf conf("rtx.raytracedRenderTargetTextures = " + hex(rt.descriptorHash) + "\n");
        const auto r = classifyOnce(into);
        CHECK(r.status == GeometryStatus::RayTraced && r.reason == ClassifyReason::DrawingToRaytracedTarget);
        CHECK(r.isDrawingToRaytracedRenderTarget && r.committed());
        CHECK(toTapDecision(r.prepareFlags) == tap::DrawDecision::Ignore); // Relight draws it
        const auto s = classifyOnce(sampling);
        CHECK(s.status == GeometryStatus::RayTraced && s.isUsingRaytracedRenderTarget);
        CHECK(s.categories.test(InstanceCategories::IgnoreOpacityMicromap));
        // A sky draw into a raytraced render target is dropped (the main sky is reused).
        D3DStateModel sky = into;
        sky.viewport.minZ = 1.0f;
        CHECK(classifyOnce(sky).reason == ClassifyReason::SkyInRaytracedTarget);
        // After injection, draws into the raytraced target are still classified.
        DrawClassifier c;
        D3DStateModel ui = worldDraw();
        ui.projection = ortho();
        ui.renderStates[d3d::RS_ZWRITEENABLE] = 0;
        CHECK(c.classify(ui).triggerRtxInjection);
        CHECK(c.classify(worldDraw()).reason == ClassifyReason::PostInjection);
        CHECK(c.classify(into).reason == ClassifyReason::DrawingToRaytracedTarget);
        ScopedConf off("rtx.raytracedRenderTarget.enable = False\n");
        CHECK(classifyOnce(into).reason == ClassifyReason::NonPrimaryTarget);
        CHECK(!classifyOnce(sampling).isUsingRaytracedRenderTarget);
    }
}

// d3d9_rtx_utils.cpp:35 isRenderTargetPrimary; d3d9_rtx.cpp:514 - non-primary RTs rasterize
// unless DXVK_RESOLUTION_WIDTH / _HEIGHT is set.
void test_primaryTarget() {
    CHECK(isRenderTargetPrimary(kBbW, kBbH, renderTarget(1, kBbW, kBbH, kFmtX8R8G8B8, false)));
    CHECK(!isRenderTargetPrimary(kBbW, kBbH, renderTarget(1, kBbW, kBbH - 1, kFmtX8R8G8B8, false)));
    D3DStateModel m = worldDraw();
    m.renderTarget0 = renderTarget(4, 512, 512, kFmtA8R8G8B8, true);
    CHECK(classifyOnce(m).status == GeometryStatus::Rasterized);
    m.resolutionOverride = true;
    CHECK(classifyOnce(m).status == GeometryStatus::RayTraced);
}

// d3d9_rtx.cpp:519-526 - stencil shadow volumes: STENCILENABLE, func ALWAYS, z-fail INCR/DECR
// (plain or saturating) and ZWRITEENABLE off are ignored.
void test_stencilShadow() {
    D3DStateModel m = worldDraw(kEmptyHash);
    m.renderStates[d3d::RS_STENCILENABLE] = 1;
    m.renderStates[d3d::RS_STENCILFUNC] = d3d::CMP_ALWAYS;
    m.renderStates[d3d::RS_ZWRITEENABLE] = 0;
    for (std::uint32_t op : {d3d::STENCILOP_INCR, d3d::STENCILOP_DECR, d3d::STENCILOP_INCRSAT, d3d::STENCILOP_DECRSAT}) {
        m.renderStates[d3d::RS_STENCILZFAIL] = op;
        CHECK(classifyOnce(m).reason == ClassifyReason::StencilShadow);
    }
    D3DStateModel zpass = m; // z-pass volumes (INCR/DECR on pass, KEEP on z-fail) are not matched
    zpass.renderStates[d3d::RS_STENCILZFAIL] = d3d::STENCILOP_KEEP;
    zpass.renderStates[d3d::RS_STENCILPASS] = d3d::STENCILOP_INCR;
    CHECK(!isStencilShadowDraw(zpass));
    D3DStateModel zwrite = m;
    zwrite.renderStates[d3d::RS_ZWRITEENABLE] = 1;
    CHECK(!isStencilShadowDraw(zwrite));
    D3DStateModel func = m;
    func.renderStates[d3d::RS_STENCILFUNC] = d3d::CMP_NOTEQUAL;
    CHECK(!isStencilShadowDraw(func));
    D3DStateModel off = m;
    off.renderStates[d3d::RS_STENCILENABLE] = 0;
    CHECK(!isStencilShadowDraw(off));
}

// d3d9_rtx.cpp:570-583 isRenderingUI and 529 - orthographic (PROJECTION[3][3] == 1) without z-write
// under FF, or a bound and sampled rtx.uiTextures texture: rasterized, triggers RTX injection.
void test_userInterface() {
    D3DStateModel m = worldDraw();
    m.projection = ortho();
    m.renderStates[d3d::RS_ZWRITEENABLE] = 0;
    auto r = classifyOnce(m);
    CHECK(r.status == GeometryStatus::Rasterized && r.triggerRtxInjection && r.reason == ClassifyReason::UserInterface);
    D3DStateModel zwrite = m;
    zwrite.renderStates[d3d::RS_ZWRITEENABLE] = 1; // ortho with z-write: not UI
    CHECK(classifyOnce(zwrite).status == GeometryStatus::RayTraced);
    D3DStateModel shader = m;
    shader.usesVertexShader = true; // the ortho test is FF only
    CHECK(classifyOnce(shader).status == GeometryStatus::RayTraced);
    {
        ScopedConf conf("rtx.orthographicIsUI = False\n");
        CHECK(classifyOnce(m).status == GeometryStatus::RayTraced);
    }
    // UI texture.
    D3DStateModel textured = worldDraw(0xAB);
    {
        ScopedConf conf("rtx.uiTextures = 0x00000000000000AB\n");
        r = classifyOnce(textured);
        CHECK(r.triggerRtxInjection && r.reason == ClassifyReason::UserInterface);
        // Remix's fixed-function sampler mask (d3d9_util.h FixedFunctionMask = 0b1111111) counts every
        // texture bound to stages 0..6, whether the stage states read it or not, but not stage 7.
        D3DStateModel unsampled = textured;
        unsampled.textureStages[0][d3d::TSS_COLOROP] = d3d::TOP_DISABLE;
        CHECK(classifyOnce(unsampled).triggerRtxInjection);
        D3DStateModel stage6 = worldDraw(kEmptyHash);
        stage6.textures[6] = texture2d(16, 0xAB);
        CHECK(classifyOnce(stage6).triggerRtxInjection);
        D3DStateModel stage7 = worldDraw(kEmptyHash);
        stage7.textures[7] = texture2d(17, 0xAB);
        CHECK(!classifyOnce(stage7).triggerRtxInjection);
        D3DStateModel ps = textured; // pixel shader declaring s0
        ps.usesPixelShader = true;
        const std::uint32_t tokens[] = {0xffff0200u, 0x0200001fu, 0x90000000u, 0xa00f0800u, 0x0000ffffu};
        ps.psSamplerMask = shaderSamplerMask(tokens, sizeof tokens);
        CHECK(classifyOnce(ps).triggerRtxInjection);
    }
    // Injection: later draws of the frame are rasterized (or dropped with
    // rtx.skipDrawCallsPostRTXInjection); the next frame starts over.
    DrawClassifier c;
    CHECK(c.classify(worldDraw()).status == GeometryStatus::RayTraced);
    CHECK(c.classify(m).triggerRtxInjection && c.rtxInjectTriggered());
    const auto post = c.classify(worldDraw());
    CHECK(post.reason == ClassifyReason::PostInjection && post.prepareFlags == prepare_draw::PreserveDrawCallAndItsState);
    CHECK(!c.classify(m).triggerRtxInjection); // injection happens once
    {
        ScopedConf conf("rtx.skipDrawCallsPostRTXInjection = True\n");
        CHECK(c.classify(worldDraw()).prepareFlags == prepare_draw::Ignore);
    }
    c.endFrame();
    CHECK(c.classify(worldDraw()).status == GeometryStatus::RayTraced);
}

// d3d9_rtx.cpp:536-546 - POSITIONT vertices: rasterized; UI (with injection) under
// rtx.preTransformedVerticesIsUI.
void test_positionT() {
    D3DStateModel m = worldDraw(kEmptyHash);
    m.hasPositionT = true;
    auto r = classifyOnce(m);
    CHECK(r.status == GeometryStatus::Rasterized && !r.triggerRtxInjection && r.reason == ClassifyReason::PositionT);
    ScopedConf conf("rtx.preTransformedVerticesIsUI = True\n");
    r = classifyOnce(m);
    CHECK(r.status == GeometryStatus::Rasterized && r.triggerRtxInjection && r.reason == ClassifyReason::PositionTAsUI);
}

// d3d9_rtx.cpp:609-614 - rtx.enableRaytracing off keeps ignored draws (PreserveDrawCallAndItsState).
void test_enableRaytracing() {
    D3DStateModel m = worldDraw();
    m.primitiveCount = 0;
    CHECK(classifyOnce(m).prepareFlags == prepare_draw::Ignore);
    ScopedConf conf("rtx.enableRaytracing = False\n");
    CHECK(classifyOnce(m).prepareFlags == prepare_draw::PreserveDrawCallAndItsState);
    CHECK(toTapDecision(classifyOnce(m).prepareFlags) == tap::DrawDecision::Raster);
}

// ---- processTextures ------------------------------------------------------------------------------------

// d3d9_rtx.cpp:917-1110 - colour texture choice: used stages up to the first COLOROP DISABLE, 2D
// textures only (cubes with rtx.allowCubemaps), lightmaps skipped, binned by texcoord index; with a
// pixel shader the first two bound samplers. Texture 0 without hash skips the draw.
void test_colorTextureSelection() {
    D3DStateModel m = worldDraw(0x10);
    m.textures[1] = texture2d(11, 0x20);
    m.textureStages[1][d3d::TSS_COLOROP] = d3d::TOP_MODULATE;
    m.textureStages[1][d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
    m.textureStages[1][d3d::TSS_COLORARG2] = d3d::TA_CURRENT;
    m.textureStages[1][d3d::TSS_TEXCOORDINDEX] = 0;
    ColorTextureSelection sel = selectColorTextures(m);
    CHECK(sel.slots[0] == 0 && sel.slots[1] == 1 && sel.texture0Hash == 0x10);
    // Stage 0 on texcoord 1, stage 1 on texcoord 0: lower texcoord index first.
    m.textureStages[0][d3d::TSS_TEXCOORDINDEX] = 1;
    sel = selectColorTextures(m);
    CHECK(sel.slots[0] == 1 && sel.slots[1] == 0 && sel.firstStage == 1 && sel.texture0Hash == 0x20);
    {
        ScopedConf conf("rtx.lightmapTextures = 0x0000000000000020\n");
        sel = selectColorTextures(m);
        CHECK(sel.slots[0] == 0 && sel.slots[1] == -1);
    }
    D3DStateModel cube = worldDraw(0x30);
    cube.textures[0].type = d3d::RTYPE_CUBETEXTURE;
    CHECK(selectColorTextures(cube).slots[0] == -1);
    {
        ScopedConf conf("rtx.allowCubemaps = True\n");
        CHECK(selectColorTextures(cube).slots[0] == 0);
    }
    D3DStateModel disabled = worldDraw(0x40);
    disabled.textureStages[0][d3d::TSS_COLOROP] = d3d::TOP_DISABLE;
    CHECK(selectColorTextures(disabled).slots[0] == -1);
    D3DStateModel ps = worldDraw(kEmptyHash);
    ps.usesPixelShader = true;
    ps.textures[0] = texture2d(12, 0x50);
    ps.textures[0].type = d3d::RTYPE_VOLUMETEXTURE; // no type check on the shader path
    CHECK(selectColorTextures(ps).slots[0] == 0);
    D3DStateModel noHash = worldDraw(0x60);
    noHash.textures[0].imageHash = kEmptyHash;
    CHECK(classifyOnce(noHash).reason == ClassifyReason::ColorTextureWithoutHash);
}

// rtx_types.cpp:376 setupCategoriesForTexture (+ d3d9_rtx.cpp:1117-1150 SmoothNormals, Ignore,
// terrain-as-decal): every texture list sets its category on the colour texture's hash.
void test_textureCategories() {
    struct Row {
        const char* option;
        InstanceCategories category;
    };
    const Row rows[] = {
        {"rtx.worldSpaceUiTextures", InstanceCategories::WorldUI},
        {"rtx.worldSpaceUiBackgroundTextures", InstanceCategories::WorldMatte},
        {"rtx.ignoreLights", InstanceCategories::IgnoreLights},
        {"rtx.antiCulling.antiCullingTextures", InstanceCategories::IgnoreAntiCulling},
        {"rtx.postfx.motionBlurMaskOutTextures", InstanceCategories::IgnoreMotionBlur},
        {"rtx.opacityMicromapIgnoreTextures", InstanceCategories::IgnoreOpacityMicromap},
        {"rtx.ignoreAlphaOnTextures", InstanceCategories::IgnoreAlphaChannel},
        {"rtx.ignoreBakedLightingTextures", InstanceCategories::IgnoreBakedLighting},
        {"rtx.hideInstanceTextures", InstanceCategories::Hidden},
        {"rtx.particleTextures", InstanceCategories::Particle},
        {"rtx.beamTextures", InstanceCategories::Beam},
        {"rtx.decalTextures", InstanceCategories::DecalStatic},
        {"rtx.animatedWaterTextures", InstanceCategories::AnimatedWater},
        {"rtx.playerModelTextures", InstanceCategories::ThirdPersonPlayerModel},
        {"rtx.playerModelBodyTextures", InstanceCategories::ThirdPersonPlayerBody},
        {"rtx.terrainTextures", InstanceCategories::Terrain},
        {"rtx.skyBoxTextures", InstanceCategories::Sky},
        {"rtx.particleEmitterTextures", InstanceCategories::ParticleEmitter},
        {"rtx.hairCardTextures", InstanceCategories::HairCards},
        {"rtx.smoothNormalsTextures", InstanceCategories::SmoothNormals},
    };
    const Hash64 h = 0x00C0FFEE12345678ull;
    CHECK(!classifyOnce(worldDraw(h)).categories.any());
    for (const Row& row : rows) {
        ScopedConf conf(std::string(row.option) + " = " + hex(h) + "\n");
        const auto r = classifyOnce(worldDraw(h));
        CHECK(r.status == GeometryStatus::RayTraced);
        CHECK(r.categories.test(row.category));
        if (!r.categories.test(row.category)) {
            std::fprintf(stderr, "  option %s -> %s\n", row.option, r.categories.toString().c_str());
        }
        CHECK(!classifyOnce(worldDraw(h + 1)).categories.test(row.category));
    }
    {
        ScopedConf conf("rtx.ignoreTextures = " + hex(h) + "\n");
        const auto r = classifyOnce(worldDraw(h));
        CHECK(r.status == GeometryStatus::Ignored && r.reason == ClassifyReason::IgnoreTexture);
        CHECK(r.categories.test(InstanceCategories::Ignore));
    }
    {
        // `-` removal in a stronger layer wins over a weaker layer's entry.
        ScopedConf weak("rtx.particleTextures = " + hex(h) + "\n");
        ScopedConf strong("rtx.particleTextures = -" + hex(h) + "\n");
        CHECK(!classifyOnce(worldDraw(h)).categories.test(InstanceCategories::Particle));
    }
    // Terrain as decals when the baker is off (d3d9_rtx.cpp:1136).
    {
        ScopedConf conf("rtx.terrainTextures = " + hex(h) +
                        "\nrtx.terrain.terrainAsDecalsEnabledIfNoBaker = True\nrtx.terrainBaker.enableBaking = False\n");
        const auto r = classifyOnce(worldDraw(h));
        CHECK(!r.categories.test(InstanceCategories::Terrain) && r.categories.test(InstanceCategories::DecalStatic));
    }
}

// rtx_options.cpp:117-145 - the deprecated decal lists migrate into rtx.decalTextures.
void test_decalMigration() {
    const Hash64 h = 0x0000D3CA10000001ull;
    ScopedConf conf("rtx.dynamicDecalTextures = " + hex(h) + "\n");
    options::OptionManager::applyPendingValues(nullptr, false); // runs the onChange migration
    CHECK(ClassifyOptions::decalTextures.containsHash(h));
    CHECK(!ClassifyOptions::dynamicDecalTextures.containsHash(h));
    const auto r = classifyOnce(worldDraw(h));
    CHECK(r.categories.test(InstanceCategories::DecalStatic) && !r.categories.test(InstanceCategories::DecalDynamic));
}

// ---- heuristics (rtx_types.cpp setupCategoriesForHeuristics) -----------------------------------------------

// shouldBakeSky explicit sources: viewport MinZ >= rtx.skyMinZThreshold, rtx.skyBoxTextures,
// rtx.skyDrawcallIdThreshold for untextured draws; setupCategoriesForGeometry: rtx.skyBoxGeometries.
void test_skyExplicit() {
    D3DStateModel m = worldDraw();
    m.viewport.minZ = 1.0f;
    m.viewport.maxZ = 1.0f;
    auto r = classifyOnce(m);
    CHECK(r.categories.test(InstanceCategories::Sky) && !r.skyAutoDetected);
    CHECK(r.prepareFlags == (prepare_draw::CommitToRayTracing | prepare_draw::ApplyDrawState));
    m.viewport.minZ = 0.5f;
    CHECK(!classifyOnce(m).categories.test(InstanceCategories::Sky));
    {
        ScopedConf conf("rtx.skyMinZThreshold = 0.5\n");
        CHECK(classifyOnce(m).categories.test(InstanceCategories::Sky));
    }
    m.viewport.minZ = 2.0f; // clamped to 1
    CHECK(classifyOnce(m).categories.test(InstanceCategories::Sky));
    {
        ScopedConf conf("rtx.skyDrawcallIdThreshold = 2\n");
        DrawClassifier c;
        CHECK(c.classify(worldDraw(kEmptyHash)).categories.test(InstanceCategories::Sky)); // id 0
        CHECK(!c.classify(worldDraw(0x77)).categories.test(InstanceCategories::Sky));      // textured: list only
        CHECK(!c.classify(worldDraw(kEmptyHash)).categories.test(InstanceCategories::Sky)); // id 2
    }
    {
        ScopedConf conf("rtx.skyBoxGeometries = 0x0000000000005C1E\n");
        DrawClassification g = classifyOnce(worldDraw());
        DrawClassifier::applyGeometryCategories(g, 0x5C1E);
        CHECK(g.categories.test(InstanceCategories::Sky));
        DrawClassification n = classifyOnce(worldDraw());
        DrawClassifier::applyGeometryCategories(n, 0x5C1F);
        CHECK(!n.categories.test(InstanceCategories::Sky));
    }
}

// checkSkyAutoDetect / makeCameraPosition: rtx.skyAutoDetect = CameraPosition(1) /
// CameraPositionAndDepthFlags(2), cameras compared by VIEW row 3 within
// rtx.skyAutoDetectUniqueCameraDistance.
void test_skyAutoDetect() {
    // makeCameraPosition exclusions.
    CHECK(!makeCameraPosition(viewAt(1, 2, 3), true, false, true));  // skinned
    CHECK(!makeCameraPosition(viewAt(1, 2, 3), false, true, false)); // particle
    CHECK(!makeCameraPosition(identityMatrix(), true, false, false));
    CHECK(makeCameraPosition(viewAt(1, 2, 3), true, false, false).has_value());

    D3DStateModel sky = worldDraw();
    sky.view = viewAt(0, 0, 0.001f); // sky camera
    sky.renderStates[d3d::RS_ZENABLE] = 0;
    D3DStateModel world = worldDraw();
    world.view = viewAt(0, -1, 50); // main camera, far from the sky camera
    CHECK(!classifyOnce(sky).categories.test(InstanceCategories::Sky)); // mode None
    {
        ScopedConf conf("rtx.skyAutoDetect = 1\n");
        DrawClassifier c;
        // Frame 0: shouldBakeSky adds the draw's camera before checkSkyAutoDetect, so one camera is
        // already seen, and the previous frame did not see two: CameraPosition falls back to "no sky".
        CHECK(!c.classify(sky).categories.test(InstanceCategories::Sky));
        CHECK(!c.classify(sky).categories.test(InstanceCategories::Sky));
        CHECK(!c.classify(world).categories.test(InstanceCategories::Sky));
        CHECK(c.seenCameraPositions().size() == 2);
        c.endFrame();
        CHECK(c.prevFrameSeenCamerasCount() == 2);
        // Frame 1: same sky camera stays sky until a new camera shows up; then nothing is sky.
        CHECK(c.classify(sky).skyAutoDetected);
        CHECK(c.classify(sky).skyAutoDetected);
        D3DStateModel identityView = worldDraw(); // no camera position: still sky
        identityView.view = identityMatrix();
        CHECK(c.classify(identityView).skyAutoDetected);
        CHECK(!c.classify(world).categories.test(InstanceCategories::Sky));
        CHECK(!c.classify(sky).categories.test(InstanceCategories::Sky));
        {
            // A larger threshold merges the two cameras: everything stays sky.
            ScopedConf far("rtx.skyAutoDetectUniqueCameraDistance = 100\n");
            DrawClassifier d;
            d.classify(sky);
            d.classify(world);
            d.endFrame();
            CHECK(d.prevFrameSeenCamerasCount() == 1);
        }
    }
    {
        ScopedConf conf("rtx.skyAutoDetect = 2\n");
        DrawClassifier c;
        CHECK(c.classify(sky).skyAutoDetected);                                  // no depth test: sky
        CHECK(!c.classify(world).categories.test(InstanceCategories::Sky));      // depth test: world
        DrawClassifier d;
        CHECK(!d.classify(world).categories.test(InstanceCategories::Sky)); // frame starts with the world
        // Raytraced render targets do not use the main view's camera list.
        std::vector<std::array<float, 3>> seen;
        SkyHeuristicInput in;
        in.zEnable = true;
        in.zWriteEnable = true;
        in.worldToView = viewAt(5, 5, 5);
        in.isDrawingToRaytracedRenderTarget = true;
        CHECK(shouldBakeSky(in, 0, seen) == SkyDetectionSource::None && seen.empty());
    }
    // checkSkyAutoDetect with two cameras already seen: never sky.
    ScopedConf conf("rtx.skyAutoDetect = 1\n");
    const std::vector<std::array<float, 3>> two = {{0, 0, 0}, {10, 0, 0}};
    CHECK(!checkSkyAutoDetect(false, std::array<float, 3>{0, 0, 0}, 2, two));
}

// shouldBakeTerrain: TerrainBaker::needsTerrainBaking() (enableBaking && non-empty
// rtx.terrainTextures) and the material hash in rtx.terrainTextures; Terrain needs the draw state.
void test_terrain() {
    const Hash64 h = 0x7E77A1Aull;
    CHECK(!shouldBakeTerrain(h));
    ScopedConf conf("rtx.terrainTextures = " + hex(h) + "\n");
    CHECK(ClassifyOptions::needsTerrainBaking());
    CHECK(shouldBakeTerrain(h) && !shouldBakeTerrain(h + 1));
    const auto r = classifyOnce(worldDraw(h));
    CHECK(r.categories.test(InstanceCategories::Terrain));
    CHECK(r.prepareFlags == (prepare_draw::CommitToRayTracing | prepare_draw::ApplyDrawState));
    ScopedConf off("rtx.terrainBaker.enableBaking = False\n");
    CHECK(!ClassifyOptions::needsTerrainBaking() && !shouldBakeTerrain(h));
    // The texture list still tags it (setupCategoriesForTexture sets Terrain regardless of the baker).
    CHECK(classifyOnce(worldDraw(h)).categories.test(InstanceCategories::Terrain));
}

// ---- helpers from DXVK / Remix -------------------------------------------------------------------------------

// DXSO sampler declarations (the programmable-shader half of D3D9ShaderMasks::samplerMask); the
// fixed-function half is Remix's constant FixedFunctionMask.
void test_samplerMasks() {
    CHECK(D3DStateModel().psSamplerMask == kFixedFunctionPsSamplerMask && kFixedFunctionPsSamplerMask == 0x7f);
    // ps_2_0: dcl_2d s3 (with a comment block first); vs_3_0: dcl_2d s1 -> slot 18.
    const std::uint32_t ps[] = {0xffff0200u, 0x0002fffeu, 0x11111111u, 0x22222222u, 0x0200001fu, 0x90000000u,
                                0xa00f0803u, 0x0000ffffu};
    CHECK(shaderSamplerMask(ps, sizeof ps) == (1u << 3));
    const std::uint32_t vs[] = {0xfffe0300u, 0x0200001fu, 0x90000000u, 0xa00f0801u, 0x0000ffffu};
    CHECK(shaderSamplerMask(vs, sizeof vs) == (1u << 18));
    const std::uint32_t ps14[] = {0xffff0104u, 0x0000ffffu};
    CHECK(shaderSamplerMask(ps14, sizeof ps14) == 0x3f);
    const std::uint32_t vs11[] = {0xfffe0101u, 0x0000ffffu};
    CHECK(shaderSamplerMask(vs11, sizeof vs11) == 0);
    CHECK(shaderSamplerMask(nullptr, 0) == (1u << tap::kSamplerSlotCount) - 1u);
}

// rtx_resources.cpp getFormatCompatibilityCategory on the D3D formats' render-target views.
void test_formatCategories() {
    using C = FormatCompatibilityCategory;
    CHECK(formatCompatibilityCategory(renderTargetVkFormat(kFmtA8R8G8B8)) == C::Color_Format_32_Bits);
    CHECK(formatCompatibilityCategory(renderTargetVkFormat(kFmtX8R8G8B8)) == C::Color_Format_32_Bits);
    CHECK(formatCompatibilityCategory(renderTargetVkFormat(113)) == C::Color_Format_64_Bits); // A16B16G16R16F
    CHECK(formatCompatibilityCategory(renderTargetVkFormat(116)) == C::Color_Format_128_Bits); // A32B32G32R32F
    CHECK(formatCompatibilityCategory(renderTargetVkFormat(50)) == C::Color_Format_8_Bits);    // L8
    CHECK(formatCompatibilityCategory(renderTargetVkFormat(kFmtNull)) == C::InvalidFormatCompatibilityCategory);
    CHECK(formatCompatibilityCategory(renderTargetVkFormat(75)) == C::InvalidFormatCompatibilityCategory); // D24S8
    CHECK(formatCompatibilityCategory(1000340000) == C::Color_Format_16_Bits);
    CHECK(formatCompatibilityCategory(121) == C::Color_Format_256_Bits);
}

// ---- tap events -> model ---------------------------------------------------------------------------------------

void test_tapTracker() {
    std::vector<ClassifiedDraw> out;
    ClassifyTap tapImpl(nullptr, [&out](const ClassifiedDraw& d) { out.push_back(d); });
    tap::DeviceEvent dev;
    dev.present.backBufferWidth = 128;
    dev.present.backBufferHeight = 96;
    tapImpl.onDeviceCreate(dev);
    tap::TextureDesc bb;
    bb.id = 1;
    bb.type = d3d::RTYPE_SURFACE;
    bb.width = 128;
    bb.height = 96;
    bb.format = kFmtX8R8G8B8;
    bb.usage = d3d::USAGE_RENDERTARGET;
    bb.isBackBuffer = true;
    bb.vkImage = 1;
    tapImpl.onTextureCreate(bb);
    tap::TextureDesc rt = bb;
    rt.id = 2;
    rt.type = d3d::RTYPE_TEXTURE;
    rt.width = rt.height = 64;
    rt.format = kFmtA8R8G8B8;
    rt.isBackBuffer = false;
    rt.mipLevels = 1;
    rt.arraySize = 1;
    tapImpl.onTextureCreate(rt);
    tap::TextureDesc tex = rt;
    tex.id = 3;
    tex.usage = 0;
    tex.pool = 1;
    tapImpl.onTextureCreate(tex);
    tapImpl.tracker().setTextureHash(3, 0xABCDEF);

    hash::TextureDescriptor desc;
    desc.width = desc.height = 64;
    desc.usage = d3d::USAGE_RENDERTARGET;
    desc.format = kFmtA8R8G8B8;
    CHECK(tapImpl.tracker().texture(2)->descriptorHash == hash::hashTextureDescriptor(desc));
    CHECK(tapImpl.tracker().texture(3)->descriptorHash == kEmptyHash);

    std::uint32_t rs[tap::kRenderStateCount] = {};
    rs[d3d::RS_ZENABLE] = 1;
    rs[d3d::RS_ZWRITEENABLE] = 1;
    rs[d3d::RS_COLORWRITEENABLE] = 0xf;
    std::uint32_t tss[tap::kTextureStageCount][32] = {};
    for (auto& st : tss) {
        st[d3d::TSS_COLOROP - 1] = d3d::TOP_DISABLE;
    }
    tss[0][d3d::TSS_COLOROP - 1] = d3d::TOP_MODULATE;
    tss[0][d3d::TSS_COLORARG1 - 1] = d3d::TA_TEXTURE;
    tss[0][d3d::TSS_COLORARG2 - 1] = d3d::TA_DIFFUSE;
    tss[0][d3d::TSS_TEXCOORDINDEX - 1] = 0;
    float transforms[tap::kTransformCount][16];
    for (auto& t : transforms) {
        std::memcpy(t, identityMatrix().data(), sizeof t);
    }
    transforms[tap::kTransformView][14] = 5.0f;
    transforms[tap::kTransformProjection][15] = 0.0f;
    transforms[tap::kTransformProjection][11] = 1.0f;
    tap::DrawState s;
    s.renderStates = rs;
    s.textureStageStates = tss;
    s.transforms = transforms;
    s.textures[0] = 3;
    s.renderTargets[0] = 1;
    s.viewport.width = 128;
    s.viewport.height = 96;
    s.elementCount = 1;
    s.elements[0].usage = static_cast<std::uint8_t>(d3d::DECLUSAGE_POSITION);
    tap::DrawCall call;
    call.call = tap::DrawCallType::DrawIndexedPrimitive;
    call.primitiveType = d3d::PT_TRIANGLELIST;
    call.primitiveCount = 2;

    const D3DStateModel m = tapImpl.tracker().buildModel(call, s);
    CHECK(m.tss(0, d3d::TSS_COLOROP) == d3d::TOP_MODULATE && m.tss(0, d3d::TSS_COLORARG1) == d3d::TA_TEXTURE);
    CHECK(m.textures[0].imageHash == 0xABCDEF && m.psSamplerMask == kFixedFunctionPsSamplerMask && m.vsSamplerMask == 0);
    CHECK(m.renderTarget0.isBackBuffer && m.backBufferWidth == 128 && m.view[14] == 5.0f && m.indexed);

    CHECK(tapImpl.onDraw(call, s) == tap::DrawDecision::Raster); // advisory by default
    CHECK(out.size() == 1 && out[0].result.status == GeometryStatus::RayTraced && out[0].result.colorTextureHash == 0xABCDEF);
    tap::QueryEvent q;
    q.type = d3d::QUERYTYPE_OCCLUSION;
    tapImpl.onQueryBegin(q);
    tapImpl.onDraw(call, s);
    CHECK(out.back().result.reason == ClassifyReason::OcclusionQuery);
    tapImpl.onQueryEnd(q);
    s.renderTargets[0] = 2; // render to texture: non-primary
    tapImpl.onDraw(call, s);
    CHECK(out.back().result.reason == ClassifyReason::NonPrimaryTarget);
    s.renderTargets[0] = 1;
    tap::FrameEvent f;
    f.frame = 0;
    tapImpl.onPresent(f);
    tapImpl.onDraw(call, s);
    CHECK(out.back().frame == 1 && out.back().indexInFrame == 0 && out.back().result.drawCallId == 0);

    ClassifyTap applying(nullptr, nullptr, true);
    applying.onDeviceCreate(dev);
    applying.onTextureCreate(bb);
    applying.onTextureCreate(tex);
    applying.tracker().setTextureHash(3, 0xABCDEF);
    CHECK(applying.onDraw(call, s) == tap::DrawDecision::Ignore); // ray traced: DXVK skips its raster
    call.primitiveType = d3d::PT_LINELIST;
    CHECK(applying.onDraw(call, s) == tap::DrawDecision::Ignore);
}

struct TestCase {
    const char* name;
    void (*fn)();
};

} // namespace

int main() {
    // Hermetic: config files and environment overrides from the caller would change the options.
    options::setEnvironmentVariable(options::kDxvkConfEnvVar, "");
    options::setEnvironmentVariable(options::kRtxConfEnvVar, "");
    options::setEnvironmentVariable("DXVK_ENABLE_RAYTRACING", "");
    (void)TerrainBakerStandIn::enableBakingObject(); // odr-use: registers the stand-in option
    options::OptionManager::applyPendingValues(nullptr, false);

    const TestCase tests[] = {
        {"drawCallRange", test_drawCallRange},
        {"vertexShaderCapture", test_vertexShaderCapture},
        {"primitives", test_primitives},
        {"alphaOptions", test_alphaOptions},
        {"occlusionQuery", test_occlusionQuery},
        {"colorTarget", test_colorTarget},
        {"shadowMask", test_shadowMask},
        {"raytracedRenderTarget", test_raytracedRenderTarget},
        {"primaryTarget", test_primaryTarget},
        {"stencilShadow", test_stencilShadow},
        {"userInterface", test_userInterface},
        {"positionT", test_positionT},
        {"enableRaytracing", test_enableRaytracing},
        {"colorTextureSelection", test_colorTextureSelection},
        {"textureCategories", test_textureCategories},
        {"decalMigration", test_decalMigration},
        {"skyExplicit", test_skyExplicit},
        {"skyAutoDetect", test_skyAutoDetect},
        {"terrain", test_terrain},
        {"samplerMasks", test_samplerMasks},
        {"formatCategories", test_formatCategories},
        {"tapTracker", test_tapTracker},
    };
    for (const TestCase& t : tests) {
        g_test = t.name;
        const int before = g_failures;
        t.fn();
        std::printf("[rl_classify] %s: %s\n", t.name, g_failures == before ? "ok" : "FAILED");
    }
    std::printf("rl_classify_unit: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
