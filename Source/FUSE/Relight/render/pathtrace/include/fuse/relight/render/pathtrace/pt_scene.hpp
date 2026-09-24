// FUSE Relight RL-5.1: the path tracer's scene (docs/plans/FUSE_REMIX_PORT_PLAN.md §5.1, §5.8; Wave R5).
//
// PtScene is the Vulkan-free description a frame hands the path tracer: object-space meshes (triangle lists with
// optional normals / texcoords / COLOR0 and per-submesh materials), instances (mesh + 3x4 object-to-world), RL-4.3
// materials with the legacy extras (texture, vertex colour, alpha test / blend, unlit, portal index), portal
// teleports, the analytic lights (RL-4.4 RlLight records: converted game lights, UsdLux / Remix lights, the fallback
// light), a uniform sky and the camera (the D3D eye and field of view; the previous frame's for motion).
//
// PtCompiledScene turns it into exactly what both path tracers read:
//   meshes      each mesh is cooked into a WP-1.2 meshlet mesh (the BLAS input of WP-6.0: GpuScene::addMeshletMesh ->
//               AccelerationStructures), so BLAS primitive t == MTRI entry t; the CPU reference uses the SAME decoded
//               (quantised) positions and triangle order (rt::RtReferenceScene), so both trace identical triangles;
//   tables      the packed words of pt_reference_core.h: one instance record per GPU-scene slot, one triangle record
//               per BLAS primitive (MTRI order; source attributes through the cook's VSRC map), one material record;
//   lights      an RL-4.4 RelightLightSet: the analytic lights first ([0, analyticCount): tested by BSDF rays), then
//               one two-sided triangle light per emissive triangle of every visible instance (radiance = the
//               material's emission), with the WP-7.1 tree; the light map sends (instance light-map base + the
//               triangle's emissive ordinal) to the light's index (-1 when the set rejected it);
//   reference   the WP-6.0 CPU acceleration-structure oracle over the cooked meshes and the instance slots.
//
// Frames: compile() builds everything (allocates); updateInstances() moves instances of the same structure (transforms
// and flags; the light set is rebuilt / refit, the reference refits) without heap allocation once warm.
#pragma once

#include "bsdf_cpp.hpp"
#include "light_cpp.hpp"

#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/renderer/geometry/meshlet_format.hpp>
#include <fuse/renderer/gpu_scene/gpu_scene_types.hpp>
#include <fuse/renderer/rt/rt_reference.hpp>
#include <fuse/types.hpp>

#include <array>
#include <string>
#include <vector>

namespace fuse::relight::render::pathtrace {

namespace bsdfk = fuse::relight::bsdf;
namespace lk = fuse::relight::lightk;
using Word = bsdfk::float4;

// Mirrors of pt_reference_core.h (the core is compiled in the C++ dialect by pt_reference.cpp only).
inline constexpr u32 kPtParamWords = 11u;
inline constexpr u32 kPtInstanceWords = 4u;
inline constexpr u32 kPtTriangleWords = 10u;
inline constexpr u32 kPtMaterialWords = 13u;
inline constexpr u32 kPtPortalWords = 3u;
inline constexpr u32 kPtMaxPortals = 16u;

enum PtFlag : u32 {
    kPtFlagJitter = 1u,
    kPtFlagNee = 2u,
    kPtFlagBsdfLights = 4u,
    kPtFlagRussianRoulette = 8u,
    kPtFlagPsr = 16u,
    kPtFlagMis = 32u,
    kPtFlagsDefault = 63u,
};

enum PtMaterialFlag : u32 {
    kPtMatVertexColor = 1u,
    kPtMatUnlit = 2u,
    kPtMatAlphaTest = 4u,
    kPtMatAlphaBlend = 8u,
    kPtMatTextured = 16u,
};

enum PtInstanceFlag : u32 {
    kPtInstanceVisible = 1u, ///< primary / secondary rays (TLAS mask kRtMaskVisible)
    kPtInstanceShadow = 2u,  ///< shadow rays (kRtMaskShadow)
};

/// Row-major 3x4 (column vectors: p' = M (p, 1)); the GpuTransform layout.
using Mat34 = std::array<float, 12>;
Mat34 identity34();
/// D3D row-vector 4x4 (p' = p M, translation in m[12..14]) -> Mat34.
Mat34 fromD3dMatrix(const float* m16);
/// Row-vector 4x4 in double (USD / RL-1.8 capture convention) -> Mat34.
Mat34 fromRowVectorMatrix(const double* m16);

struct PtSubmesh {
    u32 indexOffset = 0;
    u32 indexCount = 0;
    u32 material = 0; ///< PtScene::materials index
};

struct PtMesh {
    std::vector<float> positions; ///< xyz, object space
    std::vector<float> normals;   ///< xyz per vertex, or empty (flat shading)
    std::vector<float> uvs;       ///< uv per vertex, or empty
    std::vector<float> colors;    ///< rgba per vertex (display space rgb, as D3D COLOR0), or empty (white)
    std::vector<u32> indices;     ///< triangle list
    std::vector<PtSubmesh> submeshes; ///< empty: every triangle uses `material`
    u32 material = 0;
};

struct PtMaterial {
    bsdfk::BsdfMaterial bsdf = bsdfk::bsdfMaterialDefault();
    u32 flags = 0;          ///< PtMaterialFlag
    u32 texture = 0;        ///< bindless shader handle (kPtMatTextured); the CPU reference looks it up by value
    u32 sampler = 0;        ///< bindless sampler handle
    float alphaReference = 0.f; ///< [0, 1]
    u32 alphaCompare = 7u;  ///< VkCompareOp (ALWAYS)
    int portal = -1;        ///< bsdf.model == kBsdfModelPortal: PtScene::portals index
};

struct PtInstance {
    u32 mesh = 0;
    Mat34 objectToWorld = identity34();
    u32 flags = kPtInstanceVisible | kPtInstanceShadow;
};

struct PtCamera {
    float origin[3] = {0.f, 0.f, 0.f};
    float forward[3] = {0.f, 0.f, 1.f};
    float up[3] = {0.f, 1.f, 0.f};
    float fovY = 1.0471976f; ///< vertical field of view (radians)
    float aspect = 0.f;      ///< width / height; 0: the image's
    /// Coordinate handedness of the world: right = forward x up (right-handed, USD) or up x forward (left-handed,
    /// D3D view space), so the image's +x is the camera's right in either convention.
    bool leftHanded = false;
};

/// A CPU texture for the reference (linear-filtered RGBA as the GPU samples it; display-space values).
struct PtTextureImage {
    u32 handle = 0; ///< the bindless handle materials name
    u32 width = 0;
    u32 height = 0;
    std::vector<bsdfk::float4> texels;
};

struct PtScene {
    std::vector<PtMesh> meshes;
    std::vector<PtMaterial> materials;
    std::vector<PtInstance> instances;
    std::vector<lk::RlLight> lights; ///< analytic lights (not emissive geometry)
    std::vector<Mat34> portals;      ///< world -> world teleport of ray portal i (entering i, leaving its partner)
    std::vector<PtTextureImage> textures;
    float sky[3] = {0.f, 0.f, 0.f};
    PtCamera camera;
    bool hasPrevCamera = false;
    PtCamera prevCamera;
};

struct PtSettings {
    u32 maxBounces = 6;       ///< scattering vertices (1: direct lighting only)
    u32 rrStart = 3;          ///< first bounce with Russian roulette
    u32 flags = kPtFlagsDefault;
    u32 psrMaxBounces = 4;
    float rayEps = 1e-4f;
    float psrMirrorRoughness = 0.05f;
    u32 samplesPerPixel = 1;  ///< per frame (GPU dispatch) / per call (CPU)
    u32 maxAlphaSkips = 8;
};

struct PtCompileOptions {
    bool emissiveLights = true; ///< emissive triangles join the light set (else they are only hit by BSDF rays)
    u32 emissiveLimit = 1u << 16;
};

struct PtCompileStats {
    u32 meshes = 0;
    u32 triangles = 0;
    u32 instances = 0;
    u32 materials = 0;
    u32 analyticLights = 0;
    u32 emissiveLights = 0;
    u32 rejectedLights = 0;
};

class PtCompiledScene {
public:
    /// Cooks and packs `scene` (see the header comment). False with `error` on invalid input.
    bool compile(const PtScene& scene, const PtCompileOptions& options = {}, std::string* error = nullptr);
    /// Same meshes / instance count / meshes per instance as the compiled scene: new transforms, flags, lights,
    /// sky and cameras. False (nothing changed) when the structure differs (compile again).
    bool update(const PtScene& scene);
    /// Instance i lives in GPU-scene slot slots[i] (default: i). Re-lays the instance table and the reference.
    bool setSlots(const std::vector<u32>& slots);

    bool valid() const { return m_valid; }
    u32 meshCount() const { return static_cast<u32>(m_meshes.size()); }
    const renderer::geometry::MeshletMesh& meshlets(u32 mesh) const { return m_meshes[mesh].cooked; }
    /// Decoded (quantised) object-space positions of a cooked mesh (xyz per cooked vertex).
    const std::vector<float>& decodedPositions(u32 mesh) const { return m_meshes[mesh].decoded; }
    /// Mesh-local triangle list in MTRI order (the GPU scene's index range of the mesh).
    const std::vector<u32>& triangleIndices(u32 mesh) const { return m_meshes[mesh].indices; }
    u32 instanceCount() const { return static_cast<u32>(m_instances.size()); }
    u32 slotCount() const { return m_slotCount; }
    u32 slotOf(u32 instance) const { return m_slots[instance]; }
    const PtInstance& instance(u32 i) const { return m_instances[i]; }

    const std::vector<Word>& instanceWords() const { return m_instanceWords; }
    const std::vector<Word>& triangleWords() const { return m_triangleWords; }
    const std::vector<Word>& materialWords() const { return m_materialWords; }
    const std::vector<Word>& portalWords() const { return m_portalWords; }
    const std::vector<float>& lightMap() const { return m_lightMap; }
    const lights::RelightLightSet& lightSet() const { return m_lights; }
    const std::vector<lk::RlLight>& lightRecords() const { return m_lightRecords; }
    u32 analyticCount() const { return m_analyticCount; }
    const renderer::rt::RtReferenceScene& reference() const { return m_reference; }
    const std::vector<PtTextureImage>& textures() const { return m_textures; }
    const float* sky() const { return m_sky; }
    const PtCamera& camera() const { return m_camera; }
    const PtCamera& prevCamera() const { return m_prevCamera; }
    const PtCompileStats& stats() const { return m_stats; }

    /// The packed PtParams (pt_reference_core.h ptParamsUnpack) of a frame.
    void packParams(const PtSettings& settings, u32 width, u32 height, u32 frameSeed, u32 sampleBase,
                    Word* out) const;

private:
    struct Mesh {
        renderer::geometry::MeshletMesh cooked;
        std::vector<float> decoded;
        std::vector<u32> indices;
        u32 triangleBase = 0;
        u32 triangles = 0;
        u32 emissive = 0; ///< emissive triangles (ordinals 0..emissive-1)
        std::vector<u32> emissiveTriangles; ///< ordinal -> MTRI triangle
    };
    bool buildLights(const PtScene& scene);
    void packInstances();
    void buildReference();

    bool m_valid = false;
    PtCompileOptions m_options{};
    std::vector<Mesh> m_meshes;
    std::vector<PtInstance> m_instances;
    std::vector<PtMaterial> m_materials;
    std::vector<u32> m_slots;
    u32 m_slotCount = 0;
    std::vector<u32> m_lightMapBase; ///< per instance (-1 as ~0u: none)
    std::vector<Word> m_instanceWords;
    std::vector<Word> m_triangleWords;
    std::vector<Word> m_materialWords;
    std::vector<Word> m_portalWords;
    u32 m_portalCount = 0;
    std::vector<float> m_lightMap;
    lights::RelightLightSet m_lights;
    std::vector<lk::RlLight> m_lightRecords;
    u32 m_analyticCount = 0;
    renderer::rt::RtReferenceScene m_reference;
    std::vector<renderer::gpu_scene::GpuInstance> m_refInstances;
    std::vector<renderer::gpu_scene::GpuTransform> m_refTransforms;
    std::vector<PtTextureImage> m_textures;
    float m_sky[3] = {0.f, 0.f, 0.f};
    PtCamera m_camera;
    PtCamera m_prevCamera;
    PtCompileStats m_stats{};
};

} // namespace fuse::relight::render::pathtrace
