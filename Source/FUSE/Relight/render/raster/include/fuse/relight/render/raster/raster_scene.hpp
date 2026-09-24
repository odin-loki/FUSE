// FUSE Relight RL-4.2: the raster remaster's frame, built on the CPU at the injection point
// (docs/plans/FUSE_REMIX_PORT_PLAN.md §2.8, §5.9; Wave R4). Vulkan-free: the rl_raster_cpu_* gates test it natively.
//
// Input: the capture tap's draws before the injection point (RL-1.3 geometry, RL-1.5 fixed-function translation,
// RL-1.2 classification with the geometry categories), the frame's game lights (RL-1.5) as GPU-scene lights (their
// WP-1.1 GpuScene slots), the back buffer's clear colour. Output (RasterFrame): what the GPU half
// (raster_renderer.cpp) uploads and draws:
//
//   geometry   every scene draw's vertices, de-indexed into triangle lists (fans / strips expanded), object space,
//              with the normal (flat-shaded in the shader when the draw has none), texcoord (D3DTS_TEXTUREn applied
//              when it is not identity) and COLOR0;
//   draws      per draw objectToWorld, worldToClip (D3D row-vector VIEW x PROJECTION), the normal matrix and the
//              legacy material (below); bucketed: opaque (G-buffer, alpha test in the shader), decal (Decal*
//              categories: blended into the G-buffer albedo), blend (alpha-blended: the forward pass, in submission
//              order), skipped (points / lines, pre-transformed or programmable-VS vertices, no captured geometry);
//   camera     the main camera (the first draw the RL-1.5 camera manager classified Main; else the first scene draw):
//              eye, forward, near / far and vertical field of view from its D3D projection;
//   lights     the GPU-scene light slots: directional ones in a list, point / spot ones assigned to a 16 x 9 x 16
//              cluster grid by the renderer's WP-2.1 CPU oracle (cluster_math: buildClusterAabbs,
//              cullLightsToClusterLists, compactClusterLists) - the shaders read the rows from the GPU scene;
//              plus rtx.fallbackLight* when the frame has none;
//   fog        the first scene draw with a fog mode (Remix keeps one fog per frame);
//   shadows    (T1+) an orthographic light camera over the shadow casters' world bounds for the strongest distant
//              light.
//
// Legacy material (Remix LegacyMaterialData semantics, shaded by the RL-4.3 BSDF in the shaders): texture stage 0
// colour / alpha operation and arguments (Texture, the draw's diffuse, TFACTOR); the diffuse is COLOR0 when the
// material colour source says so, else D3DMATERIAL9 diffuse (opaque white when the material was never set); the
// emissive term D3DMATERIAL9 emissive; roughness from the specular power (Blinn-Phong n -> GGX alpha = sqrt(2 /
// (n + 2))); metallic 0. A RL-3.2 replacement material (the replacement engine's material for the draw's texture
// hash) supplies base colour (multiplies the texture), roughness, metallic and emission instead. Sky-category draws
// are unlit (their colour is emitted as is).
//
// Tiers (RendererCaps tier, capped by FUSE_RENDER_TIER_MAX and relight.raster.tier; degrades when a feature is not
// available in this build or its pipeline could not be created):
//   T0  G-buffer (legacy materials) + clustered deferred lighting (WP-2.1) + forward blended pass (WP-2.3 order and
//       legacy blend state) + decals + fog. The minimum is G-buffer + clustered.
//   T1  + directional shadow map (stand-in for the WP-3.1/3.2 virtual shadow maps, which need GPU-scene vertex
//       streams) and the WP-1.2 meshlet path (absent: the GPU scene carries no vertex streams yet -> degraded).
//   T2  + ray-query shadows / reflections (WP-6.2) and DDGI (WP-6.1): absent -> degraded to T1's features.
#pragma once

#include <fuse/relight/render/frame/scene_feed.hpp>
#include <fuse/relight/scene/translate/translate_tap.hpp>
#include <fuse/relight/tap/capture_tap.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fuse::relight::replace {
struct MaterialDef;
}

namespace fuse::relight::render::raster {

// ---- GPU records (std430; shaders/raster/raster_common.glsl mirrors them, raster_scene.cpp pins the offsets) ----

struct RasterVertex {
    float pos[3] = {0.f, 0.f, 0.f};
    std::uint32_t color = 0xffffffffu; ///< COLOR0 as RGBA8 (r in the low byte)
    float normal[3] = {0.f, 0.f, 0.f};
    std::uint32_t pad0 = 0;
    float uv[2] = {0.f, 0.f};
    std::uint32_t pad1 = 0, pad2 = 0;
};
static_assert(sizeof(RasterVertex) == 48, "RasterVertex (raster_common.glsl)");

/// RasterMaterial::flags
enum RasterMaterialFlag : std::uint32_t {
    kMatUnlit = 1u << 0,        ///< emitted as is (sky)
    kMatHasNormals = 1u << 1,   ///< else flat normals from the position derivatives
    kMatVertexColor = 1u << 2,  ///< the diffuse argument is COLOR0
    kMatAlphaTest = 1u << 3,
    kMatReplacement = 1u << 4,  ///< RL-3.2 replacement material
    kMatTextured = 1u << 5,
    kMatFog = 1u << 6,          ///< the frame's fog applies
    kMatCastShadow = 1u << 7,
};

/// RasterMaterial::ops: 4 bits each (TextureOperation / TextureArgSource values of scene/translate).
inline constexpr std::uint32_t packOps(std::uint32_t colorOp, std::uint32_t colorArg1, std::uint32_t colorArg2,
                                       std::uint32_t alphaOp, std::uint32_t alphaArg1, std::uint32_t alphaArg2) {
    return colorOp | colorArg1 << 4 | colorArg2 << 8 | alphaOp << 12 | alphaArg1 << 16 | alphaArg2 << 20;
}

struct RasterMaterial {
    float diffuse[4] = {1.f, 1.f, 1.f, 1.f};  ///< the diffuse argument when not COLOR0 (display space)
    float emissive[4] = {0.f, 0.f, 0.f, 0.f}; ///< display space (converted to linear in the shader)
    float specular[4] = {0.f, 0.f, 0.f, 0.f}; ///< rgb D3D specular colour, a unused
    float tfactor[4] = {1.f, 1.f, 1.f, 1.f};
    std::uint32_t texture = 0; ///< bindless shader handle of the colour texture (0: none -> white)
    std::uint32_t sampler = 0; ///< bindless sampler handle
    std::uint32_t ops = 0;     ///< packOps
    std::uint32_t flags = 0;   ///< RasterMaterialFlag
    std::uint32_t alphaTest = 0; ///< VkCompareOp | reference << 8
    float metallic = 0.f;
    float roughness = 1.f;       ///< perceptual
    std::uint32_t pad = 0;
};
static_assert(sizeof(RasterMaterial) == 96, "RasterMaterial (raster_common.glsl)");

struct RasterDrawGpu {
    float objectToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; ///< D3D row-major (GLSL: M * v)
    float worldToClip[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float normalToWorld[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    RasterMaterial material;
};
static_assert(sizeof(RasterDrawGpu) == 288, "RasterDrawGpu (raster_common.glsl)");

/// RasterFrameGpu::counts.w / RasterFrame::features
enum RasterFeature : std::uint32_t {
    kFeatureGBuffer = 1u << 0,
    kFeatureClustered = 1u << 1,
    kFeatureForward = 1u << 2,
    kFeatureDecals = 1u << 3,
    kFeatureFog = 1u << 4,
    kFeatureShadows = 1u << 5,
    kFeatureMeshletPath = 1u << 6,
    kFeatureRtShadows = 1u << 7,
    kFeatureRtReflections = 1u << 8,
    kFeatureDdgi = 1u << 9,
};
inline constexpr std::uint32_t kFeatureCount = 10;

struct RasterFrameGpu {
    float eye[4] = {0, 0, 0, 0};          ///< main camera position
    float forward[4] = {0, 0, 1, 0.1f};   ///< xyz main camera forward, w near
    float cluster[4] = {16, 9, 16, 1000}; ///< tiles x, tiles y, slices, far
    float screen[4] = {1, 1, 1, 1};       ///< width, height, 1 / width, 1 / height
    float fogColor[4] = {0, 0, 0, 0};     ///< rgb (display), w D3DFOGMODE
    float fogParams[4] = {0, 0, 0, 0};    ///< 1 / (end - start), end, density, enabled
    float ambient[4] = {0, 0, 0, 1};      ///< linear rgb, w exposure (scale of the lights)
    float clearColor[4] = {0, 0, 0, 1};   ///< display
    float fallbackDir[4] = {0, -1, 0, 0}; ///< xyz travel direction, w enabled
    float fallbackColor[4] = {0, 0, 0, 0};
    float shadowMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; ///< world -> shadow clip (row-major D3D)
    float shadowParams[4] = {0, 0, 0, 0}; ///< texel size, depth bias, world size of the shadow volume, enabled
    std::uint32_t counts[4] = {0, 0, 0, 0}; ///< directional count, cluster count, shadow light slot (~0: fallback), features
    std::uint32_t gbuffer[4] = {0, 0, 0, 0}; ///< albedo, normal, emissive, position (bindless texture handles)
    std::uint32_t handles[4] = {0, 0, 0, 0}; ///< specular, shadow map, nearest sampler, GPU-scene header
    std::uint64_t grid = 0;        ///< BDA: uvec2 (offset, count) per cluster
    std::uint64_t lightList = 0;   ///< BDA: GPU-scene light slots
    std::uint64_t directional = 0; ///< BDA: GPU-scene light slots
    std::uint64_t lut = 0;         ///< BDA: the RL-4.3 BSDF albedo table (kBsdfLutWords floats)
};
static_assert(sizeof(RasterFrameGpu) == 320, "RasterFrameGpu (raster_common.glsl)");

// ---- CPU side ------------------------------------------------------------------------------------------------------

enum class Bucket : std::uint8_t { Opaque = 0, Decal, Blend, Skipped };
const char* bucketName(Bucket b);

struct RasterDraw {
    std::uint32_t source = 0; ///< index in the frame's draws
    Bucket bucket = Bucket::Skipped;
    const char* skipReason = "";
    std::uint32_t firstVertex = 0, vertexCount = 0;
    std::uint32_t gpu = 0;          ///< index in RasterFrame::gpuDraws
    scene::BlendMode blend;
    bool zEnable = true, zWrite = true;
    std::uint32_t cullMode = 1;     ///< D3DCULL
    float minZ = 0.f, maxZ = 1.f;
    bool castShadow = false;
    tap::ResourceId texture = tap::kNoResource;
};

struct RasterStats {
    std::uint32_t draws = 0;         ///< scene draws considered
    std::uint32_t opaque = 0, decals = 0, blended = 0, skipped = 0, unlit = 0, textured = 0, replaced = 0;
    std::uint32_t triangles = 0, vertices = 0;
    std::uint32_t lights = 0, directional = 0, clustered = 0, lightsCulled = 0, clustersUsed = 0;
    bool fallbackLight = false;
    std::uint32_t fogMode = 0;
    bool shadow = false;
    std::uint32_t texgenIgnored = 0;
};

/// A light of the frame as the raster uses it: the GPU-scene row and its slot.
struct RasterLight {
    renderer::gpu_scene::GpuLight light;
    std::uint32_t slot = 0xffffffffu; ///< GpuScene light slot (~0: not in the GPU scene, skipped)
};

struct RasterCamera {
    bool valid = false;
    bool perspective = true;
    float eye[3] = {0, 0, 0};
    float forward[3] = {0, 0, 1};
    float up[3] = {0, 1, 0};
    float nearZ = 0.1f, farZ = 1000.f, fovY = 1.0f, aspect = 1.f;
};

struct RasterFrame {
    std::uint64_t frame = 0;
    std::uint32_t width = 0, height = 0;
    std::vector<RasterVertex> vertices;
    std::vector<RasterDrawGpu> gpuDraws;
    std::vector<RasterDraw> draws; ///< every considered draw, in submission order
    RasterFrameGpu constants;
    RasterCamera camera;
    std::vector<std::uint32_t> directional;
    std::vector<std::uint32_t> grid;      ///< 2 per cluster
    std::vector<std::uint32_t> lightList;
    std::vector<tap::ResourceId> textures; ///< sampled game textures (distinct)
    std::uint32_t shadowLight = 0xffffffffu; ///< GPU-scene slot, or ~0 with the fallback light
    bool shadow = false;
    float shadowMatrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    std::uint32_t features = 0;           ///< the effective feature mask
    RasterStats stats;
};

/// The tier plan (see the header comment).
struct TierPlan {
    std::uint32_t deviceTier = 0;
    int optionTier = -1;
    std::uint32_t tier = 0;       ///< effective
    std::uint32_t requested = 0;  ///< the tier's features
    std::uint32_t enabled = 0;    ///< requested & available
    std::string degraded;         ///< requested but unavailable, comma separated
};
/// Features a tier asks for.
std::uint32_t tierFeatures(std::uint32_t tier);
/// Features this build implements (the rest degrade).
std::uint32_t implementedFeatures();
const char* featureName(std::uint32_t bit);
/// "gbuffer,clustered,..." for a mask.
std::string featureList(std::uint32_t mask);
TierPlan planTier(std::uint32_t deviceTier, int optionTier, std::uint32_t available);

struct BuildOptions {
    std::uint32_t features = kFeatureGBuffer | kFeatureClustered;
    float ambient[3] = {0.03f, 0.03f, 0.035f};
    float exposure = 3.14159265f;
    bool fog = true;
    std::uint32_t fallbackLightMode = 1;
    float fallbackRadiance[3] = {1.6f, 1.8f, 2.0f};
    float fallbackDirection[3] = {-0.2f, -1.0f, 0.4f};
    std::uint32_t tilesX = 16, tilesY = 9, slicesZ = 16;
};

struct BuildInputs {
    std::uint64_t frame = 0;
    const std::vector<tap::CaptureDrawRecord>* draws = nullptr;
    std::uint32_t count = 0;
    const std::vector<scene::DrawClassification>* classifications = nullptr; ///< per draw in [0, count)
    const std::vector<std::uint8_t>* sceneDraw = nullptr;                   ///< per draw in [0, count)
    std::vector<RasterLight> lights;
    bool haveClear = false;
    std::uint32_t clearColor = 0; ///< D3DCOLOR
    std::uint32_t width = 0, height = 0;
    /// Bindless shader handle of a game texture (0: none).
    std::function<std::uint32_t(tap::ResourceId)> textureHandle;
    /// Bindless sampler handle for a draw's colour sampler state.
    std::function<std::uint32_t(const tap::CaptureDrawRecord::SamplerFacts&)> samplerHandle;
    /// The RL-3.2 replacement material of a legacy material hash (null: none).
    std::function<const replace::MaterialDef*(hash::Hash64)> replacementMaterial;
};

/// Builds the frame (see the header comment). False when there is nothing to draw (no scene draw survived).
bool buildRasterFrame(const BuildInputs& in, const BuildOptions& options, RasterFrame& out);

/// Legacy material mapping (exposed for the CPU gates).
RasterMaterial legacyMaterial(const scene::LegacyMaterialRecord& m, bool hasColor0, bool sky);
/// Roughness from a D3D specular power (see the header comment).
float roughnessFromPower(float power);
/// D3D row-vector matrix product a x b.
void multiplyRowMajor(const float* a, const float* b, float* out);
/// Inverse-transpose of the upper 3x3 of a row-major D3D matrix (as a 4x4, row-major). False when singular.
bool normalMatrix(const float* objectToWorld, float* out);

} // namespace fuse::relight::render::raster
