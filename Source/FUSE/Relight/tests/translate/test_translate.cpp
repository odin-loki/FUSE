// FUSE Relight RL-1.5: unit tests of fixed-function translation, game lights and cameras.
//
//   fuse_relight_translate_tests translate   (ctest rl_translate_unit)  setLegacyMaterialState,
//       setTextureStageState, textureFactorBlending, processTransforms, setFogState, FogTracker and
//       TranslateTap on synthetic tap events (d3d9_rtx_utils.cpp / d3d9_rtx.cpp / rtx_scene_manager.cpp).
//   fuse_relight_translate_tests lights      (ctest rl_lights_unit)  D3DLIGHT9 -> light record, the stable
//       light hash (rtx_lights_data.cpp), calculateIntensity (rtx_light_utils.cpp), the light options and
//       LightTranslator (addGameLight, "off" lights, same-frame rule, dirty detection).
//   fuse_relight_translate_tests camera      (ctest rl_camera_kat)  decomposeProjection known-answer tests
//       within 1e-5 (MathLib DecomposeProjection), jitter detection and CameraManager::processCameraData.
// Options are set through RL-0.6 layers built from rtx.conf text, as a game's rtx.conf would set them.
#include <fuse/relight/scene/translate/translate_options.hpp>
#include <fuse/relight/scene/translate/translate_tap.hpp>

#include <fuse/relight/scene/camera/camera_options.hpp>
#include <fuse/relight/scene/classify/classify_options.hpp>
#include <fuse/relight/scene/lights/light_options.hpp>

#include <fuse/relight/hash/xxh.hpp>
#include <fuse/relight/options/option_config.hpp>
#include <fuse/relight/options/option_manager.hpp>

#include <cmath>
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

#define CHECK_NEAR(a, b, tol)                                                                                  \
    do {                                                                                                       \
        ++g_checks;                                                                                            \
        const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);                              \
        if (!(std::abs(va_ - vb_) <= (tol) * std::max(1.0, std::abs(vb_)))) {                                   \
            ++g_failures;                                                                                      \
            std::fprintf(stderr, "FAIL %s:%d [%s]: %s = %.9g, expected %.9g (tol %g)\n", __FILE__, __LINE__, g_test, \
                         #a, va_, vb_, static_cast<double>(tol));                                             \
        }                                                                                                      \
    } while (0)

using namespace fuse::relight;
using namespace fuse::relight::scene;

/// Stand-ins for options other packages own (the borrowed-by-name reads must see them).
struct BorrowedStandIns {
    FUSE_RELIGHT_OPTION("rtx", float, sceneScale, 1.f, "Test stand-in for the scene package's option.");
    FUSE_RELIGHT_OPTION("rtx", float, uniqueObjectDistance, 300.f, "Test stand-in for the instances package's option.");
    FUSE_RELIGHT_OPTION("rtx", bool, useWorldMatricesForShaders, true, "Test stand-in for the vertex capture option.");
};

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
        m_layer = options::OptionManager::acquireLayer(
            "", {6000u + static_cast<std::uint32_t>(s_counter++), "rl_translate_test"}, 1.0f, 0.1f, false, &config);
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

std::uint32_t f2u(float f) {
    std::uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

Mat4 identity() { return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; }

/// D3DXMatrixPerspectiveFovLH / RH.
Mat4 perspectiveFov(float fovY, float aspect, float zn, float zf, bool lh) {
    const float yScale = 1.0f / std::tan(fovY / 2.0f);
    const float xScale = yScale / aspect;
    Mat4 m{};
    m[0] = xScale;
    m[5] = yScale;
    if (lh) {
        m[10] = zf / (zf - zn);
        m[11] = 1.0f;
        m[14] = -zn * zf / (zf - zn);
    } else {
        m[10] = zf / (zn - zf);
        m[11] = -1.0f;
        m[14] = zn * zf / (zn - zf);
    }
    return m;
}

/// D3DXMatrixLookAtLH.
Mat4 lookAtLH(const std::array<float, 3>& eye, const std::array<float, 3>& at, const std::array<float, 3>& up) {
    auto sub = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[0] - b[0], a[1] - b[1], a[2] - b[2]};
    };
    auto norm = [](std::array<float, 3> v) {
        const float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        return std::array<float, 3>{v[0] / l, v[1] / l, v[2] / l};
    };
    auto cross = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::array<float, 3>{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
    };
    auto dot = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    };
    const auto z = norm(sub(at, eye));
    const auto x = norm(cross(up, z));
    const auto y = cross(z, x);
    return {x[0], y[0], z[0], 0, x[1], y[1], z[1], 0, x[2], y[2], z[2], 0, -dot(x, eye), -dot(y, eye), -dot(z, eye), 1};
}

D3DStateModel baseModel() {
    D3DStateModel m;
    setD3D9DefaultStates(m);
    m.renderStates[d3dff::RS_TEXTUREFACTOR] = 0xffffffffu;
    m.renderStates[d3dff::RS_SRCBLEND] = d3dff::BLEND_ONE;
    m.renderStates[d3dff::RS_DESTBLEND] = d3dff::BLEND_ZERO;
    m.renderStates[d3dff::RS_BLENDOP] = d3dff::BLENDOP_ADD;
    m.renderStates[d3dff::RS_ALPHAFUNC] = d3dff::CMP_ALWAYS;
    m.renderStates[d3d::RS_COLORWRITEENABLE] = 0xf;
    m.renderStates[d3dff::RS_LIGHTING] = 1;
    m.renderStates[d3dff::RS_COLORVERTEX] = 1;
    m.renderStates[d3dff::RS_DIFFUSEMATERIALSOURCE] = d3dff::MCS_COLOR1;
    m.renderStates[d3dff::RS_SPECULARMATERIALSOURCE] = d3dff::MCS_COLOR2;
    m.renderStates[d3dff::RS_FOGEND] = f2u(1.0f);
    m.renderStates[d3dff::RS_FOGDENSITY] = f2u(1.0f);
    m.view = identity();
    m.projection = perspectiveFov(1.0f, 4.0f / 3.0f, 0.5f, 100.f, true);
    m.world = identity();
    return m;
}

TextureRecord texture2d(tap::ResourceId id, hash::Hash64 h) {
    TextureRecord t;
    t.id = id;
    t.type = d3d::RTYPE_TEXTURE;
    t.width = t.height = 64;
    t.format = 21;
    t.imageHash = h;
    return t;
}

// ==================================================================================================
// translate
// ==================================================================================================

void test_colorSources() {
    FixedFunctionState ff;
    // Lighting off, vertex colours present: COLOR1 -> VertexColor0, COLOR2 unsupported -> None.
    D3DStateModel m = baseModel();
    m.renderStates[d3dff::RS_LIGHTING] = 0;
    ff.hasColor0 = ff.hasColor1 = true;
    LegacyMaterialRecord r = setLegacyMaterialState(m, ff, false);
    CHECK(r.diffuseColorSource == TextureArgSource::VertexColor0);
    CHECK(r.specularColorSource == TextureArgSource::None);
    // No vertex colour: material.
    ff.hasColor0 = ff.hasColor1 = false;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.diffuseColorSource == TextureArgSource::None);
    // Lighting on: DIFFUSEMATERIALSOURCE masked by the available colours.
    m.renderStates[d3dff::RS_LIGHTING] = 1;
    ff.hasColor0 = true;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.diffuseColorSource == TextureArgSource::VertexColor0);
    m.renderStates[d3dff::RS_DIFFUSEMATERIALSOURCE] = d3dff::MCS_MATERIAL;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.diffuseColorSource == TextureArgSource::None);
    // DIFFUSEMATERIALSOURCE = COLOR2 with only COLOR0 in the declaration: 2 & 1 = 0 -> material.
    m.renderStates[d3dff::RS_DIFFUSEMATERIALSOURCE] = d3dff::MCS_COLOR2;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.diffuseColorSource == TextureArgSource::None);
    // COLORVERTEX off: mask 0.
    m.renderStates[d3dff::RS_DIFFUSEMATERIALSOURCE] = d3dff::MCS_COLOR1;
    m.renderStates[d3dff::RS_COLORVERTEX] = 0;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.diffuseColorSource == TextureArgSource::None);
    // POSITIONT disables FF lighting: the vertex colour is used whatever the material sources say.
    m.hasPositionT = true;
    m.renderStates[d3dff::RS_DIFFUSEMATERIALSOURCE] = d3dff::MCS_MATERIAL;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.diffuseColorSource == TextureArgSource::VertexColor0);
}

void test_alphaTest() {
    FixedFunctionState ff;
    D3DStateModel m = baseModel();
    LegacyMaterialRecord r = setLegacyMaterialState(m, ff, false);
    CHECK(!r.alphaTestEnabled);
    CHECK(r.alphaTestCompareOp == vk::COMPARE_OP_ALWAYS);
    m.renderStates[d3d::RS_ALPHATESTENABLE] = 1;
    m.renderStates[d3dff::RS_ALPHAFUNC] = d3dff::CMP_GREATER;
    m.renderStates[d3dff::RS_ALPHAREF] = 0x1280; // only the low 8 bits count
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.alphaTestEnabled);
    CHECK(r.alphaTestCompareOp == vk::COMPARE_OP_GREATER);
    CHECK(r.alphaTestReferenceValue == 0x80);
    m.renderStates[d3dff::RS_ALPHAFUNC] = d3dff::CMP_LESSEQUAL;
    CHECK(setLegacyMaterialState(m, ff, false).alphaTestCompareOp == vk::COMPARE_OP_LESS_OR_EQUAL);
    // Every D3DCMPFUNC.
    const std::uint32_t expect[9] = {vk::COMPARE_OP_NEVER, vk::COMPARE_OP_NEVER, vk::COMPARE_OP_LESS, vk::COMPARE_OP_EQUAL,
                                     vk::COMPARE_OP_LESS_OR_EQUAL, vk::COMPARE_OP_GREATER, vk::COMPARE_OP_NOT_EQUAL,
                                     vk::COMPARE_OP_GREATER_OR_EQUAL, vk::COMPARE_OP_ALWAYS};
    for (std::uint32_t f = 0; f < 9; ++f) {
        CHECK(decodeCompareOp(f) == expect[f]);
    }
    // Alpha to coverage (ALPHATESTENABLE + ADAPTIVETESS_Y = ATOC on NVIDIA, multisampled target) is not an
    // alpha test.
    m.renderStates[d3d::RS_ADAPTIVETESS_Y] = d3d::FMT_ATOC;
    m.renderTarget0 = texture2d(9, 0);
    m.renderTarget0.multiSample = d3d::MULTISAMPLE_2_SAMPLES;
    CHECK(!setLegacyMaterialState(m, ff, false).alphaTestEnabled);
}

void test_blendState() {
    FixedFunctionState ff;
    D3DStateModel m = baseModel();
    m.renderStates[d3d::RS_ALPHABLENDENABLE] = 1;
    m.renderStates[d3dff::RS_SRCBLEND] = d3dff::BLEND_SRCALPHA;
    m.renderStates[d3dff::RS_DESTBLEND] = d3dff::BLEND_INVSRCALPHA;
    m.renderStates[d3dff::RS_BLENDOP] = d3dff::BLENDOP_REVSUBTRACT;
    LegacyMaterialRecord r = setLegacyMaterialState(m, ff, false);
    CHECK(r.blendMode.enableBlending);
    CHECK(r.blendMode.colorSrcFactor == vk::BLEND_FACTOR_SRC_ALPHA);
    CHECK(r.blendMode.colorDstFactor == vk::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    CHECK(r.blendMode.colorBlendOp == vk::BLEND_OP_REVERSE_SUBTRACT);
    // Without separate alpha the alpha channel follows the colour blend.
    CHECK(r.blendMode.alphaSrcFactor == vk::BLEND_FACTOR_SRC_ALPHA);
    CHECK(r.blendMode.alphaBlendOp == vk::BLEND_OP_REVERSE_SUBTRACT);
    // Separate alpha.
    m.renderStates[d3dff::RS_SEPARATEALPHABLENDENABLE] = 1;
    m.renderStates[d3dff::RS_SRCBLENDALPHA] = d3dff::BLEND_ONE;
    m.renderStates[d3dff::RS_DESTBLENDALPHA] = d3dff::BLEND_ZERO;
    m.renderStates[d3dff::RS_BLENDOPALPHA] = d3dff::BLENDOP_MAX;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.blendMode.alphaSrcFactor == vk::BLEND_FACTOR_ONE);
    CHECK(r.blendMode.alphaDstFactor == vk::BLEND_FACTOR_ZERO);
    CHECK(r.blendMode.alphaBlendOp == vk::BLEND_OP_MAX);
    CHECK(r.blendMode.colorBlendOp == vk::BLEND_OP_REVERSE_SUBTRACT);
    // BOTHSRCALPHA / BOTHINVSRCALPHA fix-up (the destination is overridden).
    m.renderStates[d3dff::RS_SEPARATEALPHABLENDENABLE] = 0;
    m.renderStates[d3dff::RS_SRCBLEND] = d3dff::BLEND_BOTHINVSRCALPHA;
    m.renderStates[d3dff::RS_DESTBLEND] = d3dff::BLEND_ONE;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.blendMode.colorSrcFactor == vk::BLEND_FACTOR_ONE_MINUS_SRC_ALPHA);
    CHECK(r.blendMode.colorDstFactor == vk::BLEND_FACTOR_SRC_ALPHA);
    // BLENDFACTOR: colour vs alpha constant.
    m.renderStates[d3dff::RS_SRCBLEND] = d3dff::BLEND_BLENDFACTOR;
    m.renderStates[d3dff::RS_DESTBLEND] = d3dff::BLEND_INVBLENDFACTOR;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.blendMode.colorSrcFactor == vk::BLEND_FACTOR_CONSTANT_COLOR);
    CHECK(r.blendMode.alphaSrcFactor == vk::BLEND_FACTOR_CONSTANT_ALPHA);
    CHECK(r.blendMode.alphaDstFactor == vk::BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA);
    // Alpha-swizzled render target: destination alpha reads one.
    m.renderStates[d3dff::RS_SRCBLEND] = d3dff::BLEND_DESTALPHA;
    m.renderStates[d3dff::RS_DESTBLEND] = d3dff::BLEND_INVDESTALPHA;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.blendMode.colorSrcFactor == vk::BLEND_FACTOR_DST_ALPHA);
    r = setLegacyMaterialState(m, ff, true);
    CHECK(r.blendMode.colorSrcFactor == vk::BLEND_FACTOR_ONE);
    CHECK(r.blendMode.colorDstFactor == vk::BLEND_FACTOR_ZERO);
    CHECK(r.blendMode.alphaSrcFactor == vk::BLEND_FACTOR_ONE);
    // Unknown values decode to the DXVK defaults.
    CHECK(decodeBlendFactor(99, false) == vk::BLEND_FACTOR_ZERO);
    CHECK(decodeBlendOp(0) == vk::BLEND_OP_ADD);
    // Write mask, texture factor, D3DMATERIAL9.
    m.renderStates[d3d::RS_COLORWRITEENABLE] = 0x7;
    m.renderStates[d3dff::RS_TEXTUREFACTOR] = 0x80402010u;
    ff.material.diffuse = {0.1f, 0.2f, 0.3f, 0.4f};
    ff.material.power = 12.f;
    r = setLegacyMaterialState(m, ff, false);
    CHECK(r.blendMode.writeMask == 0x7);
    CHECK(r.tFactor == 0x80402010u);
    CHECK(r.d3dMaterial.diffuse.g == 0.2f && r.d3dMaterial.power == 12.f);
    CHECK(r.hash() == hash::kEmptyHash);
    // rtx.vertexColorIsBakedLighting.
    CHECK(r.isVertexColorBakedLighting);
    {
        ScopedConf conf("rtx.vertexColorIsBakedLighting = False\n");
        CHECK(!setLegacyMaterialState(m, ff, false).isVertexColorBakedLighting);
    }
}

void test_alphaSwizzleFormats() {
    CHECK(renderTargetHasAlphaSwizzle(22));  // X8R8G8B8
    CHECK(renderTargetHasAlphaSwizzle(24));  // X1R5G5B5
    CHECK(!renderTargetHasAlphaSwizzle(21)); // A8R8G8B8
    CHECK(!renderTargetHasAlphaSwizzle(23)); // R5G6B5 (no alpha channel, identity swizzle)
    CHECK(!renderTargetHasAlphaSwizzle(26)); // A4R4G4B4
    CHECK(renderTargetHasAlphaSwizzle(114)); // R32F
    CHECK(renderTargetHasAlphaSwizzle(d3d::fourcc('A', 'T', 'I', '2')));
}

void test_textureStage() {
    FixedFunctionState ff;
    for (auto& t : ff.textureTransforms) {
        t = identity();
    }
    ff.textureTransforms[1][12] = 0.5f;
    D3DStateModel m = baseModel();
    ff.hasColor0 = ff.hasColor1 = true;
    m.renderStates[d3dff::RS_LIGHTING] = 0;
    LegacyMaterialRecord r = setLegacyMaterialState(m, ff, false);
    DrawTransforms t = processTransforms(m, ff, true);
    // Stage 1: MODULATE2X of TEXTURE and DIFFUSE; alpha SELECTARG2 of SPECULAR.
    m.textureStages[1][d3d::TSS_COLOROP] = d3dff::TOP_MODULATE2X;
    m.textureStages[1][d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
    m.textureStages[1][d3d::TSS_COLORARG2] = d3d::TA_DIFFUSE;
    m.textureStages[1][d3d::TSS_ALPHAOP] = d3d::TOP_SELECTARG2;
    m.textureStages[1][d3d::TSS_ALPHAARG1] = d3d::TA_TEXTURE | 0x10; // D3DTA_COMPLEMENT: not supported -> None
    m.textureStages[1][d3d::TSS_ALPHAARG2] = d3dff::TA_SPECULAR;
    m.textureStages[1][d3dff::TSS_TEXTURETRANSFORMFLAGS] = 2; // D3DTTFF_COUNT2
    m.textureStages[1][d3d::TSS_TEXCOORDINDEX] = d3dff::TCI_CAMERASPACEPOSITION;
    setTextureStageState(m, ff, 1, true, false, r, t);
    CHECK(r.textureColorOperation == TextureOperation::Modulate2x);
    CHECK(r.textureColorArg1Source == TextureArgSource::Texture);
    CHECK(r.textureColorArg2Source == TextureArgSource::VertexColor0);
    CHECK(r.textureAlphaOperation == TextureOperation::SelectArg2);
    CHECK(r.textureAlphaArg1Source == TextureArgSource::None);
    CHECK(r.textureAlphaArg2Source == TextureArgSource::None); // SPECULAR -> specular source (COLOR2: None)
    CHECK(t.textureTransform[12] == 0.5f);
    CHECK(t.texgenMode == TexGenMode::ViewPositions);
    // Unsupported ops fall back to Modulate; TFACTOR dropped without stage texture-factor blending.
    m.textureStages[1][d3d::TSS_COLOROP] = d3d::TOP_LERP;
    m.textureStages[1][d3d::TSS_COLORARG1] = d3d::TA_TFACTOR;
    m.textureStages[1][d3dff::TSS_TEXTURETRANSFORMFLAGS] = d3dff::TTFF_DISABLE;
    m.textureStages[1][d3d::TSS_TEXCOORDINDEX] = d3dff::TCI_CAMERASPACENORMAL | 1;
    setTextureStageState(m, ff, 1, true, true, r, t);
    CHECK(r.textureColorOperation == TextureOperation::Modulate);
    CHECK(r.textureColorArg1Source == TextureArgSource::TFactor);
    CHECK(r.isTextureFactorBlend);
    CHECK(t.textureTransform == identity());
    CHECK(t.texgenMode == TexGenMode::None); // the full value (with an index) matches no TCI case
    setTextureStageState(m, ff, 1, false, false, r, t);
    CHECK(r.textureColorArg1Source == TextureArgSource::None);
    CHECK(!r.isTextureFactorBlend);
    m.textureStages[1][d3d::TSS_TEXCOORDINDEX] = d3dff::TCI_CAMERASPACENORMAL;
    setTextureStageState(m, ff, 1, false, false, r, t);
    CHECK(t.texgenMode == TexGenMode::ViewNormals);
    m.textureStages[1][d3d::TSS_TEXCOORDINDEX] = d3dff::TCI_SPHEREMAP;
    setTextureStageState(m, ff, 1, false, false, r, t);
    CHECK(t.texgenMode == TexGenMode::None);
}

void test_textureFactorBlending() {
    D3DStateModel m = baseModel();
    m.textures[0] = texture2d(1, 0x1111);
    m.textures[1] = texture2d(2, 0x2222);
    // Stage 0: MODULATE TEXTURE x CURRENT; stage 1: MODULATE TFACTOR x CURRENT.
    m.textureStages[0][d3d::TSS_COLOROP] = d3d::TOP_MODULATE;
    m.textureStages[0][d3d::TSS_COLORARG1] = d3d::TA_TEXTURE;
    m.textureStages[0][d3d::TSS_COLORARG2] = d3d::TA_DIFFUSE;
    m.textureStages[1][d3d::TSS_COLOROP] = d3d::TOP_MODULATE;
    m.textureStages[1][d3d::TSS_COLORARG1] = d3d::TA_TFACTOR;
    m.textureStages[1][d3d::TSS_COLORARG2] = d3d::TA_CURRENT;
    TextureFactorBlending tf = textureFactorBlending(m);
    CHECK(tf.useStageTextureFactorBlending);
    CHECK(tf.useMultipleStageTextureFactorBlending);
    {
        ScopedConf conf("rtx.enableMultiStageTextureFactorBlending = False\n");
        CHECK(!textureFactorBlending(m).useMultipleStageTextureFactorBlending);
    }
    // A TFACTOR stage reading TEMP when the previous stage did not write TEMP is not a TF blend.
    m.textureStages[1][d3d::TSS_COLORARG2] = d3d::TA_TEMP;
    CHECK(!textureFactorBlending(m).useMultipleStageTextureFactorBlending);
    m.textureStages[0][d3d::TSS_RESULTARG] = d3d::TA_TEMP;
    CHECK(textureFactorBlending(m).useMultipleStageTextureFactorBlending);
    // rtx.ignoreBakedLightingTextures on a TF-blended stage disables texture-factor blending.
    m.textureStages[0][d3d::TSS_RESULTARG] = d3d::TA_CURRENT;
    m.textureStages[1][d3d::TSS_COLORARG2] = d3d::TA_CURRENT;
    {
        ScopedConf conf("rtx.ignoreBakedLightingTextures = 0x2222\n");
        tf = textureFactorBlending(m);
        CHECK(!tf.useStageTextureFactorBlending);
        CHECK(!tf.useMultipleStageTextureFactorBlending);
    }
    // Programmable pixel shader: defaults.
    m.usesPixelShader = true;
    tf = textureFactorBlending(m);
    CHECK(tf.useStageTextureFactorBlending && !tf.useMultipleStageTextureFactorBlending);
}

void test_terrainDecalModulate() {
    LegacyMaterialRecord r;
    r.textureColorOperation = TextureOperation::Modulate4x;
    applyTerrainAsDecalModulate(0x77, r);
    CHECK(r.textureColorOperation == TextureOperation::Modulate4x);
    ScopedConf conf("rtx.terrainTextures = 0x77\nrtx.terrain.terrainAsDecalsEnabledIfNoBaker = True\n"
                    "rtx.terrainBaker.enableBaking = False\nrtx.terrain.terrainAsDecalsAllowOverModulate = True\n");
    applyTerrainAsDecalModulate(0x77, r);
    CHECK(r.textureColorOperation == TextureOperation::Force_Modulate2x);
    r.textureColorOperation = TextureOperation::Modulate;
    applyTerrainAsDecalModulate(0x77, r);
    CHECK(r.textureColorOperation == TextureOperation::Modulate);
}

void test_transforms() {
    FixedFunctionState ff;
    D3DStateModel m = baseModel();
    m.world = identity();
    m.world[12] = 5.f;
    m.view = identity();
    m.view[14] = 2.f;
    DrawTransforms t = processTransforms(m, ff, true);
    CHECK(t.objectToWorld[12] == 5.f);
    CHECK(t.objectToView[12] == 5.f && t.objectToView[14] == 2.f);
    // Programmable VS: world trusted only with vertex capture and rtx.useWorldMatricesForShaders.
    m.usesVertexShader = true;
    CHECK(processTransforms(m, ff, true).objectToWorld[12] == 5.f);
    CHECK(processTransforms(m, ff, false).objectToWorld == identity());
    {
        ScopedConf conf("rtx.useWorldMatricesForShaders = False\n");
        CHECK(processTransforms(m, ff, true).objectToWorld == identity());
    }
    m.usesVertexShader = false;
    // sanitize: [3][3] == 0 becomes 1.
    m.world[15] = 0.f;
    m.view[15] = 0.f;
    t = processTransforms(m, ff, true);
    CHECK(t.objectToWorld[15] == 1.f && t.worldToView[15] == 1.f && t.objectToView[15] == 1.f);
    // Clip planes: the first enabled non-degenerate one.
    ff.clipPlanes[0] = {1, 0, 0, 1};
    ff.clipPlanes[1] = {0, 0, 0, 3};
    ff.clipPlanes[2] = {0, 1, 0, 2};
    m.renderStates[d3dff::RS_CLIPPLANEENABLE] = 0b110;
    t = processTransforms(m, ff, true);
    CHECK(t.enableClipPlane);
    CHECK(t.clipPlane[1] == 1.f && t.clipPlane[3] == 2.f);
    m.renderStates[d3dff::RS_CLIPPLANEENABLE] = 0;
    CHECK(!processTransforms(m, ff, true).enableClipPlane);
    // multiply is the row-vector product.
    Mat4 a = identity(), b = identity();
    a[12] = 1.f;
    b[0] = 2.f;
    const Mat4 ab = multiply(a, b);
    CHECK(ab[0] == 2.f && ab[12] == 2.f);
}

void test_fog() {
    D3DStateModel m = baseModel();
    FogRecord f = setFogState(m);
    CHECK(f.mode == d3dff::FOG_NONE);
    m.renderStates[d3dff::RS_FOGENABLE] = 1;
    m.renderStates[d3dff::RS_FOGCOLOR] = 0xff8090a0u;
    m.renderStates[d3dff::RS_FOGVERTEXMODE] = d3dff::FOG_LINEAR;
    m.renderStates[d3dff::RS_FOGSTART] = f2u(2.0f);
    m.renderStates[d3dff::RS_FOGEND] = f2u(22.0f);
    m.renderStates[d3dff::RS_FOGDENSITY] = f2u(0.25f);
    f = setFogState(m);
    CHECK(f.mode == d3dff::FOG_LINEAR);
    CHECK(f.color[0] == 128.0f / 255.0f && f.color[1] == 144.0f / 255.0f && f.color[2] == 160.0f / 255.0f);
    CHECK(f.scale == 1.0f / 20.0f);
    CHECK(f.end == 22.0f && f.density == 0.25f);
    // Table mode wins over vertex mode.
    m.renderStates[d3dff::RS_FOGTABLEMODE] = d3dff::FOG_EXP2;
    CHECK(setFogState(m).mode == d3dff::FOG_EXP2);
    // FogState::getHash: XXH3 over the packed 28-byte struct.
    std::uint8_t bytes[28];
    const std::uint32_t mode = f.mode;
    std::memcpy(bytes, &mode, 4);
    std::memcpy(bytes + 4, f.color.data(), 12);
    std::memcpy(bytes + 16, &f.scale, 4);
    std::memcpy(bytes + 20, &f.end, 4);
    std::memcpy(bytes + 24, &f.density, 4);
    CHECK(f.hash() == hash::xxh3_64(bytes, 28));
    // FogTracker: first fog of the frame, distinct states.
    FogTracker tracker;
    FogRecord none;
    tracker.processDraw(none);
    tracker.processDraw(f);
    FogRecord g = f;
    g.density = 0.5f;
    tracker.processDraw(g);
    tracker.processDraw(f);
    CHECK(tracker.frameStates().size() == 2);
    CHECK(tracker.frameFog().hash() == f.hash());
    tracker.endFrame();
    CHECK(tracker.frameFog().mode == d3dff::FOG_NONE && tracker.frameStates().empty());
}

/// A minimal tap session: device, back buffer, one draw per call to draw().
struct TapHarness {
    std::vector<TranslatedDraw> draws;
    std::vector<TranslatedFrame> frames;
    TranslateTap tap{nullptr, [this](const TranslatedDraw& d) { draws.push_back(d); },
                     [this](const TranslatedFrame& f) { frames.push_back(f); }};
    std::uint32_t rs[tap::kRenderStateCount] = {};
    std::uint32_t tss[tap::kTextureStageCount][32] = {};
    std::vector<float> transforms = std::vector<float>(tap::kTransformCount * 16, 0.f);
    std::vector<tap::Light> lights;

    TapHarness() {
        tap::DeviceEvent e;
        e.present.backBufferWidth = 128;
        e.present.backBufferHeight = 96;
        e.backBuffer = 1;
        tap.onDeviceCreate(e);
        tap::TextureDesc bb;
        bb.id = 1;
        bb.type = d3d::RTYPE_SURFACE;
        bb.width = 128;
        bb.height = 96;
        bb.depth = 1;
        bb.mipLevels = 1;
        bb.arraySize = 1;
        bb.format = 22; // X8R8G8B8
        bb.usage = d3d::USAGE_RENDERTARGET;
        bb.isBackBuffer = true;
        bb.vkImage = 1;
        tap.onTextureCreate(bb);
        D3DStateModel defaults = baseModel();
        std::memcpy(rs, defaults.renderStates.data(), sizeof rs);
        for (std::uint32_t s = 0; s < tap::kTextureStageCount; ++s) {
            for (std::uint32_t i = 0; i + 1 < kTextureStageStateSlots; ++i) {
                tss[s][i] = defaults.textureStages[s][i + 1];
            }
        }
        for (std::uint32_t t = 0; t < tap::kTransformCount; ++t) {
            for (std::uint32_t k = 0; k < 4; ++k) {
                transforms[t * 16 + k * 5] = 1.f;
            }
        }
        setTransform(tap::kTransformProjection, perspectiveFov(1.0f, 4.0f / 3.0f, 0.5f, 100.f, true));
        setTransform(tap::kTransformView, lookAtLH({0.f, 1.f, -5.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}));
    }
    void setTransform(std::uint32_t slot, const Mat4& m) { std::memcpy(&transforms[slot * 16], m.data(), 64); }
    void draw() {
        tap::DrawCall c;
        c.call = tap::DrawCallType::DrawPrimitive;
        c.primitiveType = d3d::PT_TRIANGLELIST;
        c.primitiveCount = 1;
        c.vertexCount = 3;
        tap::DrawState s;
        s.renderStates = rs;
        s.textureStageStates = tss;
        s.transforms = reinterpret_cast<const float(*)[16]>(transforms.data());
        s.lights = lights.empty() ? nullptr : lights.data();
        s.lightCount = static_cast<std::uint32_t>(lights.size());
        s.viewport = {0, 0, 128, 96, 0.f, 1.f};
        s.renderTargets[0] = 1;
        s.elementCount = 2;
        s.elements[0] = {0, 0, 2, 0, d3d::DECLUSAGE_POSITION, 0};
        s.elements[1] = {0, 12, 4, 0, d3dff::DECLUSAGE_COLOR, 0};
        tap.onDraw(c, s);
    }
    void present() { tap.onPresent(tap::FrameEvent{}); }
};

void test_translateTap() {
    TapHarness h;
    tap::Light point;
    point.index = 0;
    point.enabled = true;
    point.type = d3dlight::POINT;
    point.diffuse = {1.f, 0.5f, 0.25f, 1.f};
    point.position = {1.f, 2.f, 3.f};
    point.range = 10.f;
    point.attenuation0 = 1.f;
    h.lights.push_back(point);
    h.rs[d3dff::RS_FOGENABLE] = 1;
    h.rs[d3dff::RS_FOGVERTEXMODE] = d3dff::FOG_LINEAR;
    h.draw();
    h.draw();
    h.present();
    CHECK(h.draws.size() == 2);
    CHECK(h.draws[0].translated && h.draws[0].textureStageApplied);
    CHECK(h.draws[0].alphaSwizzle); // X8R8G8B8 back buffer
    CHECK(h.draws[0].material.diffuseColorSource == TextureArgSource::VertexColor0);
    CHECK(h.draws[0].addedLights.size() == 1);
    CHECK(h.draws[1].addedLights.empty()); // not dirty
    CHECK(h.draws[0].cameraType == CameraType::Main);
    CHECK(h.draws[0].fog.mode == d3dff::FOG_LINEAR);
    CHECK(h.frames.size() == 1);
    CHECK(h.frames[0].lights.size() == 1);
    CHECK(h.frames[0].lights[0].hash == stableLightHash(point));
    CHECK(h.frames[0].fogStates.size() == 1);
    CHECK(h.frames[0].cameras.size() == 1 && h.frames[0].cameras[0].type == CameraType::Main);
    CHECK_NEAR(h.frames[0].cameras[0].fov, 1.0, 1e-5);
    CHECK_NEAR(h.frames[0].cameras[0].position()[2], -5.0, 1e-5);
    // Next frame: the unchanged light is re-sent at the first processed draw.
    h.draw();
    CHECK(h.draws[2].addedLights.size() == 1);
    // A rasterized draw (no colour writes) is neither translated nor given a camera.
    h.rs[d3d::RS_COLORWRITEENABLE] = 0;
    h.draw();
    CHECK(!h.draws[3].translated && h.draws[3].cameraType == CameraType::Unknown);
    // fogIgnoreSky: a sky draw (skyMinZThreshold) has no fog.
    h.rs[d3d::RS_COLORWRITEENABLE] = 0xf;
    {
        ScopedConf conf("rtx.fogIgnoreSky = True\nrtx.skyMinZThreshold = 0\n");
        h.draw();
        CHECK(h.draws[4].classification.categories.test(InstanceCategories::Sky));
        CHECK(h.draws[4].fog.mode == d3dff::FOG_NONE);
        CHECK(h.draws[4].cameraType == CameraType::Sky);
    }
    h.present();
    CHECK(h.frames.size() == 2);
}

// ==================================================================================================
// lights
// ==================================================================================================

tap::Light makeLight(std::uint32_t type) {
    tap::Light l;
    l.enabled = true;
    l.type = type;
    l.diffuse = {1.f, 0.3f, 0.2f, 1.f};
    l.position = {-1.6f, 1.f, -1.f};
    l.direction = {-0.3f, -1.f, 0.6f};
    l.range = 6.f;
    l.attenuation0 = 0.2f;
    l.attenuation1 = 0.3f;
    l.attenuation2 = 0.05f;
    l.theta = 0.35f;
    l.phi = 0.9f;
    l.falloff = 1.f;
    return l;
}

void test_stableHash() {
    // Point: sphere hash of the raw position and radius 4, no shaping.
    tap::Light p = makeLight(d3dlight::POINT);
    hash::LightShaping none;
    CHECK(stableLightHash(p) == hash::hashSphereLight({-1.6f, 1.f, -1.f}, 4.0f, none));
    // The options do not change the stable hash.
    {
        ScopedConf conf("rtx.lightConversionSphereLightFixedRadius = 9\n");
        CHECK(convertLegacyLight(p)->hash == hash::hashSphereLight({-1.6f, 1.f, -1.f}, 4.0f, none));
        CHECK(convertLegacyLight(p)->radius == 9.f);
    }
    // Spot: shaping from the raw direction, cos(phi / 2), cos(theta / 2) - cos(phi / 2), falloff.
    tap::Light s = makeLight(d3dlight::SPOT);
    hash::LightShaping sh;
    sh.enabled = true;
    sh.direction = {-0.3f, -1.f, 0.6f};
    sh.cosConeAngle = static_cast<float>(std::cos(0.45));
    sh.coneSoftness = static_cast<float>(std::cos(static_cast<double>(0.35f / 2.0f))) - sh.cosConeAngle;
    sh.focusExponent = 1.f;
    CHECK(stableLightHash(s) == hash::hashSphereLight({-1.6f, 1.f, -1.f}, 4.0f, sh));
    // Directional: seeded with RtLightType::Rect (1), raw direction, half angle 0.0349 / 2.
    tap::Light d = makeLight(d3dlight::DIRECTIONAL);
    const float dir[3] = {-0.3f, -1.f, 0.6f};
    const float half = 0.0349f / 2.0f;
    hash::Hash64 h = 1;
    h = hash::xxh64(dir, 12, h);
    h = hash::xxh64(&half, 4, h);
    CHECK(stableLightHash(d) == h);
    CHECK(stableLightHash(d) != hash::hashDistantLight({-0.3f, -1.f, 0.6f}, half));
    // Invalid type.
    CHECK(stableLightHash(makeLight(7)) == hash::kEmptyHash);
    CHECK(!convertLegacyLight(makeLight(0)).has_value());
    CHECK(!convertLegacyLight(makeLight(4)).has_value());
}

void test_conversion() {
    // Point light with only a constant term: end distance = Range.
    tap::Light p = makeLight(d3dlight::POINT);
    p.attenuation0 = 1.f;
    p.attenuation1 = p.attenuation2 = 0.f;
    std::optional<LightRecord> r = convertLegacyLight(p);
    CHECK(r && r->type == hash::LightType::Sphere);
    const float k = kNewLightEndValue / (kLightPi * 4.f * 4.f);
    CHECK(r->intensity == k * (6.f * 6.f));
    CHECK(r->radius == 4.f);
    CHECK(!r->shaping.enabled);
    // Radiance = diffuse / max component * intensity.
    CHECK_NEAR(r->radiance[0], r->intensity, 1e-7);
    CHECK_NEAR(r->radiance[1], 0.3 * r->intensity, 1e-6);
    // Constant term too large: radiance 0 -> "off".
    p.attenuation0 = 300.f;
    CHECK(convertLegacyLight(p)->isOff());
    // Linear falloff without least squares: (brightness / (1/255) - c) / b.
    p = makeLight(d3dlight::POINT);
    p.attenuation0 = 1.f;
    p.attenuation1 = 0.5f;
    p.attenuation2 = 0.f;
    {
        ScopedConf conf("rtx.calculateLightIntensityUsingLeastSquares = False\n");
        const float end = ((1.0f / kLegacyLightEndValue) - 1.f) / 0.5f;
        CHECK(calculateLegacyLightIntensity(p, 4.f) == k * (end * end));
    }
    // Least squares (default) gives a positive intensity lower than the single-point fit for this curve.
    const float ls = calculateLegacyLightIntensity(p, 4.f);
    CHECK(ls > 0.f);
    // Intensity factor and max intensity.
    {
        ScopedConf conf("rtx.lightConversionIntensityFactor = 2\n");
        CHECK(calculateLegacyLightIntensity(p, 4.f) == ls * 2.f);
    }
    {
        ScopedConf conf("rtx.lightConversionMaxIntensity = 0.001\n");
        CHECK(calculateLegacyLightIntensity(p, 4.f) == 0.001f);
    }
    // rtx.sceneScale (borrowed) scales the sphere radius.
    {
        ScopedConf conf("rtx.sceneScale = 2\n");
        CHECK(convertLegacyLight(p)->radius == 8.f);
    }
    // Spot: normalized axis, cone phi / 2.
    tap::Light s = makeLight(d3dlight::SPOT);
    r = convertLegacyLight(s);
    CHECK(r->shaping.enabled);
    const float len = std::sqrt(0.09f + 1.f + 0.36f);
    CHECK_NEAR(r->shaping.direction[1], -1.f / len, 1e-6);
    CHECK(r->shaping.cosConeAngle == static_cast<float>(std::cos(0.45)));
    CHECK(r->shaping.focusExponent == 1.f);
    // Zero spot direction falls back to +Z.
    s.direction = {0.f, 0.f, 0.f};
    CHECK(convertLegacyLight(s)->shaping.direction == (Float3{0.f, 0.f, 1.f}));
    // Directional: fixed intensity and angle, unnormalized colour.
    tap::Light d = makeLight(d3dlight::DIRECTIONAL);
    d.diffuse = {0.8f, 0.8f, 0.75f, 1.f};
    r = convertLegacyLight(d);
    CHECK(r->type == hash::LightType::Distant);
    CHECK(r->halfAngle == 0.0349f / 2.0f);
    CHECK(r->radiance == (Float3{0.8f, 0.8f, 0.75f}));
    CHECK_NEAR(r->direction[0] * r->direction[0] + r->direction[1] * r->direction[1] + r->direction[2] * r->direction[2], 1.0, 1e-6);
    {
        ScopedConf conf("rtx.lightConversionDistantLightFixedIntensity = 3\nrtx.lightConversionDistantLightFixedAngle = 0.1\n");
        r = convertLegacyLight(d);
        CHECK(r->radiance[2] == 0.75f * 3.f);
        CHECK(r->halfAngle == 0.05f);
    }
    // Negative (subtractive) light: off.
    d.diffuse = {-1.f, 0.f, 0.f, 1.f};
    CHECK(convertLegacyLight(d)->isOff());
}

void test_lightTransform() {
    // The rotation of a spot / directional light maps -Z onto the light direction (row-vector: -row 2).
    const tap::Vec3 dirs[4] = {{0.f, 0.f, 1.f}, {0.f, 0.f, -1.f}, {-0.3f, -1.f, 0.6f}, {1.f, 0.f, 0.f}};
    for (const tap::Vec3& dv : dirs) {
        tap::Light s = makeLight(d3dlight::SPOT);
        s.direction = dv;
        const std::array<float, 16> m = legacyLightTransform(s);
        const Float3 z = safeNormalize({dv.x, dv.y, dv.z}, {0.f, 0.f, 1.f});
        CHECK_NEAR(-m[8], z[0], 1e-5);
        CHECK_NEAR(-m[9], z[1], 1e-5);
        CHECK_NEAR(-m[10], z[2], 1e-5);
        CHECK(m[12] == s.position.x && m[14] == s.position.z);
    }
    tap::Light p = makeLight(d3dlight::POINT);
    const std::array<float, 16> m = legacyLightTransform(p);
    CHECK(m[0] == 1.f && m[13] == 1.f);
}

void test_lightTranslator() {
    LightTranslator t;
    std::vector<tap::Light> lights = {makeLight(d3dlight::DIRECTIONAL), makeLight(d3dlight::POINT),
                                      makeLight(d3dlight::SPOT), makeLight(d3dlight::POINT)};
    for (std::uint32_t i = 0; i < lights.size(); ++i) {
        lights[i].index = i;
    }
    lights[3].enabled = false; // set but disabled
    std::vector<LightRecord> added = t.processDraw(lights.data(), 4);
    CHECK(added.size() == 3);
    CHECK(t.processDraw(lights.data(), 4).empty()); // not dirty
    // Same light re-set with a different index in the same frame: same hash -> ignored.
    lights[3] = lights[1];
    lights[3].index = 3;
    added = t.processDraw(lights.data(), 4);
    CHECK(added.empty());
    CHECK(t.frameLights().size() == 3);
    // A moved light is a new light.
    lights[1].position.x = 5.f;
    added = t.processDraw(lights.data(), 4);
    CHECK(added.size() == 1 && added[0].position[0] == 5.f);
    t.endFrame();
    CHECK(t.frameLights().empty());
    // Type filters.
    {
        ScopedConf conf("rtx.ignoreGamePointLights = True\nrtx.ignoreGameDirectionalLights = True\n");
        added = t.processDraw(lights.data(), 4);
        CHECK(added.size() == 1 && added[0].d3dType == d3dlight::SPOT);
        CHECK(t.frameRejected() == 3);
    }
    t.endFrame();
    // Off lights are rejected.
    lights[0].diffuse = {0.f, 0.f, 0.f, 1.f};
    added = t.processDraw(lights.data(), 1);
    CHECK(added.empty() && t.frameRejected() == 1);
}

// ==================================================================================================
// camera (KATs within 1e-5)
// ==================================================================================================

constexpr double kKat = 1e-5;

void test_decomposeKat() {
    struct Case {
        float fovY, aspect, zn, zf;
    };
    const Case cases[] = {{1.04719758f, 4.f / 3.f, 0.5f, 100.f}, {0.785398163f, 16.f / 9.f, 0.1f, 1000.f},
                          {1.57079633f, 1.f, 1.f, 10.f},         {0.2f, 2.35f, 4.f, 50000.f},
                          {2.5f, 0.75f, 0.01f, 0.5f}};
    for (const Case& c : cases) {
        for (int lh = 0; lh < 2; ++lh) {
            const Mat4 m = perspectiveFov(c.fovY, c.aspect, c.zn, c.zf, lh != 0);
            const DecomposeProjectionParams p = decomposeProjection(m);
            // The planes the float matrix encodes (D3D: z_ndc = (z * m22 + m32) / (z * m23)), in double: a
            // float m22 close to 1 (zf >> zn) cannot encode zf exactly, so the reference is the matrix's own.
            const double m22 = m[10], m32 = m[14], m23 = m[11];
            const double zNear = -m32 / m22 * (m23 > 0 ? 1.0 : -1.0);
            const double zFar = m32 / (m23 - m22) * (m23 > 0 ? 1.0 : -1.0);
            CHECK_NEAR(p.fov, c.fovY, kKat);
            CHECK_NEAR(p.aspectRatio, c.aspect, kKat);
            CHECK_NEAR(p.nearPlane, zNear, kKat);
            CHECK_NEAR(p.farPlane, zFar, kKat);
            CHECK_NEAR(zNear, c.zn, 1e-5);
            CHECK_NEAR(zFar, c.zf, 2e-3);
            CHECK_NEAR(p.shearX, 0.0, kKat);
            CHECK_NEAR(p.shearY, 0.0, kKat);
            CHECK(p.isLHS == (lh != 0));
            CHECK(!p.isReverseZ && !p.isOrthographic);
        }
    }
    // Reversed Z (LH, near and far swapped in the depth mapping).
    Mat4 rz = perspectiveFov(1.0f, 1.5f, 0.25f, 200.f, true);
    rz[10] = 0.25f / (0.25f - 200.f);
    rz[14] = -200.f * 0.25f / (0.25f - 200.f);
    DecomposeProjectionParams p = decomposeProjection(rz);
    CHECK(p.isReverseZ);
    CHECK_NEAR(p.nearPlane, 0.25, kKat);
    CHECK_NEAR(p.farPlane, 200.0, kKat);
    CHECK_NEAR(p.fov, 1.0, kKat);
    // Infinite far plane (LH): D22 = 1, D32 = -zn.
    Mat4 inf = perspectiveFov(0.9f, 1.25f, 0.3f, 100.f, true);
    inf[10] = 1.f;
    inf[14] = -0.3f;
    p = decomposeProjection(inf);
    CHECK_NEAR(p.nearPlane, 0.3, kKat);
    CHECK_NEAR(p.fov, 0.9, kKat);
    CHECK(p.farPlane > 1e30f);
    // Mirrored projection (m00 < 0): MathLib's aspect is negative, and rtx_matrix_helpers.h flips the sign
    // when m00 * m11 <= 0, so the reported aspect ratio stays positive.
    Mat4 mir = perspectiveFov(1.0f, 1.5f, 1.f, 10.f, true);
    mir[0] = -mir[0];
    CHECK_NEAR(decomposeProjection(mir).aspectRatio, 1.5, kKat);
    CHECK_NEAR(decomposeProjection(mir).fov, 1.0, kKat);
    // Orthographic (D3DXMatrixOrthoLH 8 x 6, 1..11): no FOV.
    Mat4 ortho{};
    ortho[0] = 2.f / 8.f;
    ortho[5] = 2.f / 6.f;
    ortho[10] = 1.f / 10.f;
    ortho[14] = -1.f / 10.f;
    ortho[15] = 1.f;
    p = decomposeProjection(ortho);
    CHECK(p.isOrthographic);
    CHECK(p.fov == 0.f);
    CHECK_NEAR(p.nearPlane, 1.0, kKat);
    CHECK_NEAR(p.farPlane, 11.0, kKat);
    CHECK_NEAR(p.aspectRatio, 8.0 / 6.0, kKat);
    // Off-centre (sheared) perspective: shear = mean of the frustum angles.
    Mat4 off = perspectiveFov(1.0f, 1.f, 1.f, 10.f, true);
    off[8] = 0.2f; // x offset in NDC
    p = decomposeProjection(off);
    const double xs = 1.0 / off[0];
    // LH: the frustum spans x in [-(1 - 0.2), 1 + 0.2] * xs at z = 1.
    const double expectShear = 0.5 * (std::atan((1.0 + 0.2) * xs) - std::atan((1.0 - 0.2) * xs));
    CHECK_NEAR(p.shearX, expectShear, kKat);
    CHECK_NEAR(p.fov, 1.0, kKat);
}

void test_jitter() {
    Mat4 m = perspectiveFov(1.0f, 4.f / 3.f, 0.5f, 100.f, true);
    ProjectionJitter j = projectionJitter(m, 1280, 720);
    CHECK(!j.subPixel && j.pixelX == 0.f);
    // TAA jitter of (+0.25, -0.5) pixels at 1280 x 720.
    m[8] = 0.25f * 2.f / 1280.f;
    m[9] = 0.5f * 2.f / 720.f;
    j = projectionJitter(m, 1280, 720);
    CHECK(j.subPixel);
    CHECK_NEAR(j.pixelX, 0.25, kKat);
    CHECK_NEAR(j.pixelY, -0.5, kKat);
    // RH projections (m[11] = -1) flip the sign of the NDC offset.
    Mat4 rh = perspectiveFov(1.0f, 4.f / 3.f, 0.5f, 100.f, false);
    rh[8] = 0.001f;
    CHECK_NEAR(projectionJitter(rh, 1280, 720).ndcX, -0.001, kKat);
    // A large off-centre term is not jitter.
    m[8] = 0.5f;
    CHECK(!projectionJitter(m, 1280, 720).subPixel);
    // removeProjectionJitter restores the centred projection.
    const Mat4 clean = removeProjectionJitter(m);
    CHECK(clean[8] == 0.f && clean[9] == 0.f);
    CHECK_NEAR(decomposeProjection(clean).shearX, 0.0, kKat);
}

CameraDrawInput cameraInput(const Mat4& view, const Mat4& proj) {
    CameraDrawInput in;
    in.worldToView = view;
    in.viewToProjection = proj;
    in.objectToWorld = identity();
    in.objectToView = multiply(in.objectToWorld, view);
    in.viewportWidth = 1280;
    in.viewportHeight = 720;
    return in;
}

void test_cameraManager() {
    CameraManager cm;
    const Mat4 proj = perspectiveFov(1.04719758f, 4.f / 3.f, 0.5f, 100.f, true);
    const Mat4 view = lookAtLH({0.f, 0.6f, -4.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f});
    CameraDrawInput in = cameraInput(view, proj);
    CHECK(cm.processCameraData(in, 0) == CameraType::Main);
    const CameraState& main = cm.getMainCamera();
    CHECK(main.isValid(0));
    CHECK_NEAR(main.fov, 1.04719758, kKat);
    CHECK_NEAR(main.aspectRatio, 4.0 / 3.0, kKat);
    CHECK_NEAR(main.nearPlane, 0.5, kKat);
    CHECK_NEAR(main.farPlane, 100.0, kKat);
    CHECK_NEAR(main.position()[1], 0.6, kKat);
    CHECK_NEAR(main.position()[2], -4.0, kKat);
    CHECK(main.direction()[2] > 0.9f); // LHS: looking down +Z
    CHECK(!cm.isCameraCutThisFrame(0));
    // Later draws of the frame do not update the camera.
    CameraDrawInput moved = cameraInput(lookAtLH({0.f, 0.6f, -8.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}), proj);
    CHECK(cm.processCameraData(moved, 0) == CameraType::Main);
    CHECK_NEAR(cm.getMainCamera().position()[2], -4.0, kKat);
    cm.onFrameEnd();
    // Frame 1: a teleport beyond rtx.uniqueObjectDistance (300) is a camera cut.
    CameraDrawInput far = cameraInput(lookAtLH({0.f, 0.6f, -400.f}, {0.f, 0.f, 0.f}, {0.f, 1.f, 0.f}), proj);
    CHECK(cm.processCameraData(far, 1) == CameraType::Main);
    CHECK(cm.isCameraCutThisFrame(1));
    {
        ScopedConf conf("rtx.uniqueObjectDistance = 1000\n");
        CHECK(cm.processCameraData(cameraInput(view, proj), 2) == CameraType::Main);
        CHECK(!cm.isCameraCutThisFrame(2));
    }
    // FOV change between frames is counted.
    CHECK(cm.fovChanges() == 0);
    CHECK(cm.processCameraData(cameraInput(view, perspectiveFov(0.5f, 4.f / 3.f, 0.5f, 100.f, true)), 3) == CameraType::Main);
    CHECK(cm.fovChanges() == 1);
    // No camera: identity projection; fused world-view rule (objectToView == objectToWorld, not identity).
    CHECK(cm.processCameraData(cameraInput(view, identity()), 3) == CameraType::Unknown);
    CameraDrawInput fused = cameraInput(identity(), proj);
    fused.objectToWorld = view;
    fused.objectToView = view;
    CHECK(cm.processCameraData(fused, 3) == CameraType::Unknown);
    {
        ScopedConf conf("rtx.fusedWorldViewMode = 1\n");
        CHECK(cm.processCameraData(fused, 4) == CameraType::Main);
    }
    fused.isSky = true;
    CHECK(cm.processCameraData(fused, 4) == CameraType::Sky); // no camera, but a sky draw
    // Invalid camera: shear beyond 0.01 rad.
    Mat4 sheared = proj;
    sheared[8] = 0.2f;
    const std::uint32_t rejected = cm.rejectedCameras();
    CHECK(cm.processCameraData(cameraInput(view, sheared), 4) == CameraType::Unknown);
    CHECK(cm.rejectedCameras() == rejected + 1);
    // Sky and render-to-texture cameras.
    CameraDrawInput sky = cameraInput(view, proj);
    sky.isSky = true;
    CHECK(cm.processCameraData(sky, 5) == CameraType::Sky);
    CHECK(cm.isCameraValid(CameraType::Sky, 5));
    CameraDrawInput rtt = cameraInput(view, proj);
    rtt.isDrawingToRaytracedRenderTarget = true;
    rtt.isSky = true;
    CHECK(cm.processCameraData(rtt, 5) == CameraType::RenderToTexture);
    // View model: needs rtx.viewModel.enable; maxZ threshold or a FOV different from the Main camera.
    CameraDrawInput vm = cameraInput(view, perspectiveFov(0.7f, 4.f / 3.f, 0.1f, 10.f, true));
    CHECK(cm.processCameraData(cameraInput(view, proj), 6) == CameraType::Main);
    CHECK(cm.processCameraData(vm, 6) == CameraType::Main);
    {
        ScopedConf conf("rtx.viewModel.enable = True\n");
        CHECK(cm.processCameraData(vm, 6) == CameraType::ViewModel);
        CameraDrawInput vmz = cameraInput(view, proj);
        vmz.maxZ = 0.0f;
        CHECK(cm.processCameraData(vmz, 6) == CameraType::ViewModel);
        vmz.maxZ = 1.0f;
        CHECK(cm.processCameraData(vmz, 6) == CameraType::Main);
    }
    // Unknown reads the Main camera.
    CHECK(&cm.getCamera(CameraType::Unknown) == &cm.getMainCamera());
    // Jitter detection: sub-pixel offsets changing between frames.
    CameraManager jm;
    Mat4 jp = proj;
    for (std::uint32_t f = 0; f < 4; ++f) {
        jp[8] = (f % 2 ? 0.25f : -0.25f) * 2.f / 1280.f;
        jp[9] = (f % 2 ? -0.25f : 0.25f) * 2.f / 720.f;
        CHECK(jm.processCameraData(cameraInput(view, jp), f) == CameraType::Main);
        CHECK(jm.getMainCamera().jitter.subPixel);
        CHECK(jm.getMainCamera().jitterDetected == (f > 0));
        CHECK_NEAR(jm.getMainCamera().fov, 1.04719758, 1e-4);
        jm.onFrameEnd();
    }
}

void test_inverse() {
    const Mat4 view = lookAtLH({3.f, 2.f, -7.f}, {0.5f, 0.f, 1.f}, {0.f, 1.f, 0.f});
    const std::array<double, 16> inv = inverseMatrix(view);
    for (std::size_t i = 0; i < 4; ++i) {
        for (std::size_t j = 0; j < 4; ++j) {
            double s = 0;
            for (std::size_t k = 0; k < 4; ++k) {
                s += static_cast<double>(view[i * 4 + k]) * inv[k * 4 + j];
            }
            CHECK_NEAR(s, i == j ? 1.0 : 0.0, 1e-6);
        }
    }
    CHECK_NEAR(inv[12], 3.0, 1e-5);
    CHECK_NEAR(inv[14], -7.0, 1e-5);
    CHECK(isIdentityExact(identity()));
    Mat4 almost = identity();
    almost[3] = -0.0f; // -0 == 0
    CHECK(isIdentityExact(almost));
}

struct TestCase {
    const char* name;
    void (*fn)();
};

int run(const char* group, const TestCase* tests, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        g_test = tests[i].name;
        tests[i].fn();
    }
    if (g_failures) {
        std::fprintf(stderr, "%s: %d of %d check(s) failed\n", group, g_failures, g_checks);
        return 1;
    }
    std::printf("%s: %d check(s) passed in %zu test(s)\n", group, g_checks, count);
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // Hermetic: config files and environment overrides from the caller would change the options.
    options::setEnvironmentVariable(options::kDxvkConfEnvVar, "");
    options::setEnvironmentVariable(options::kRtxConfEnvVar, "");
    options::setEnvironmentVariable("RTX_ENABLE_MULTI_STAGE_TEXTURE_FACTOR_BLENDING", "");
    options::setEnvironmentVariable("DXVK_LEGACY_MATERIAL_DEFAULT_ROUGHNESS", "");
    options::setEnvironmentVariable("DXVK_ENABLE_RAYTRACING", "");
    // odr-use: registers the stand-in options.
    (void)BorrowedStandIns::sceneScaleObject();
    (void)BorrowedStandIns::uniqueObjectDistanceObject();
    (void)BorrowedStandIns::useWorldMatricesForShadersObject();
    (void)TerrainBakerStandIn::enableBakingObject();
    options::OptionManager::applyPendingValues(nullptr, false);

    const std::string group = argc > 1 ? argv[1] : "all";
    const TestCase translate[] = {
        {"colorSources", test_colorSources},     {"alphaTest", test_alphaTest},
        {"blendState", test_blendState},         {"alphaSwizzleFormats", test_alphaSwizzleFormats},
        {"textureStage", test_textureStage},     {"textureFactorBlending", test_textureFactorBlending},
        {"terrainDecalModulate", test_terrainDecalModulate}, {"transforms", test_transforms},
        {"fog", test_fog},                       {"translateTap", test_translateTap},
    };
    const TestCase lights[] = {
        {"stableHash", test_stableHash},
        {"conversion", test_conversion},
        {"lightTransform", test_lightTransform},
        {"lightTranslator", test_lightTranslator},
    };
    const TestCase camera[] = {
        {"decomposeKat", test_decomposeKat},
        {"jitter", test_jitter},
        {"cameraManager", test_cameraManager},
        {"inverse", test_inverse},
    };
    int rc = 0;
    if (group == "translate" || group == "all") {
        rc |= run("rl_translate_unit", translate, std::size(translate));
    }
    if (group == "lights" || group == "all") {
        rc |= run("rl_lights_unit", lights, std::size(lights));
    }
    if (group == "camera" || group == "all") {
        rc |= run("rl_camera_kat", camera, std::size(camera));
    }
    // Leave no option dirty at exit (see rl_classify_replay).
    options::OptionManager::applyPendingValues(nullptr, false);
    return rc;
}
