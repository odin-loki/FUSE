// FUSE Relight RL-6.2: the runtime behind the C APIs (docs/plans/FUSE_REMIX_PORT_PLAN.md §1.10, Wave R6).
//
// One process-wide ApiRuntime (global()) serves both front ends: the FUSE-native fuse_relight_* functions
// (fuse_relight_api.h) and the Remix API 0.6 compatible remixapi_Interface (remixapi_compat.h). It owns the API's
// objects and builds one API frame per EndFrame / Present on the existing Relight systems:
//
//   CreateMaterial   RL-3.2 MaterialParams (sanitized to the upstream ranges) -> RL-4.3 bsdfMaterialFromParams (the
//                    emission drives the emissive-triangle lights)
//   CreateMesh       per surface: positions / normals / texcoords / indices copied; the RL-1.3 hash components
//                    (positions and texcoords over the used vertices, indices, geometry descriptor, vertex layout)
//                    and the asset hash under rtx.geometryAssetHashRuleString; the object-space bounding box
//   CreateLight      RL-4.4 RlLight (lightFromUsd for UsdLux parameters, or the typed constructors)
//   DrawInstance     queued; at the frame end every surface is one RL-1.7 SceneModel draw (stable instance id,
//                    transform history), then one RL-3.4 ReplacementEngine lookup when an engine is attached (mesh by
//                    the surface's asset hash, material by the API material's hash); kept emissive surfaces add their
//                    triangles to the RL-4.4 RelightLightSet
//   DrawLight        queued; added to the light set at the frame end (after the emissive triangles' draws)
//   SetOption        RL-0.6 option layer "Relight API" (priority kMaxDynamicLayerPriority); pending values are applied
//                    (OptionManager::applyPendingValues) at the start of the next frame end, so that frame uses them
//   endFrame         replacement beginFrame -> draws -> replacement endFrame -> light set build -> SceneModel endFrame
//                    (camera: the last SetCamera of the frame) -> FrameRecord
//
// Renderer hook (not wired here: render/frame is owned by RL-4.1 / RL-5.1): lastFrame() is the finished API frame
// (draws with their instance ids, transforms and BSDF materials; the light set). RenderTap's scene feed can append
// ApiFrame::draws to its AdapterDraws and ApiRuntime::lights() to its AdapterLights at the injection point, and call
// endFrame() from its Present when the application never calls Present / EndFrame itself (see the RL-6.2 row).
#pragma once

#include <fuse/relight/api/fuse_relight_api.h>
#include <fuse/relight/hash/geometry_hash.hpp>
#include <fuse/relight/mods/import/light_table.hpp>
#include <fuse/relight/mods/import/material_table.hpp>
#include <fuse/relight/render/lights/light_set.hpp>
#include <fuse/relight/render/material/material_bsdf.hpp>
#include <fuse/relight/scene/instances/scene_model.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace fuse::relight::replace {
class ReplacementEngine;
struct EngineConfig;
}
namespace fuse::relight::options {
class OptionLayerHandle;
}

namespace fuse::relight::api {

using Handle = std::uint64_t;
namespace lk = fuse::relight::lightk;

struct ApiMaterial {
    std::uint64_t hash = 0;
    mods::import::MaterialParams params;
    render::material::bsdf::BsdfMaterial bsdf{};
    lk::float3 emission{0.f, 0.f, 0.f}; ///< bsdf.emission (radiance of emissive triangles)
};

struct ApiSurface {
    std::vector<float> positions; ///< xyz
    std::vector<float> normals;   ///< xyz or empty
    std::vector<float> texcoords; ///< uv or empty
    std::vector<std::uint32_t> indices; ///< triangle list (non-indexed input: 0..n-1)
    Handle material = 0;
    hash::GeometryHashes hashes;
    hash::Hash64 assetHash = 0;
    scene::instances::AxisAlignedBoundingBox bounds;
};

struct ApiMesh {
    std::uint64_t hash = 0;
    std::vector<ApiSurface> surfaces;
};

struct ApiLight {
    std::uint64_t hash = 0;
    lk::RlLight light{};
    bool supported = true; ///< false: accepted but not representable (dome lights); never enters the light set
};

struct ApiInstance {
    Handle mesh = 0;
    std::uint32_t apiCategories = 0; ///< REMIXAPI_INSTANCE_CATEGORY_BIT_*
    scene::instances::Mat4f objectToWorld = scene::instances::identityMatrix(); ///< row vectors (p' = p M)
    bool doubleSided = false;
};

struct ApiCamera {
    std::uint32_t type = 0; ///< 0 main, 1 sky, 2 view model
    scene::instances::Mat4f view = scene::instances::identityMatrix();
    scene::instances::Mat4f projection = scene::instances::identityMatrix();
};

/// One surface draw of the finished frame.
struct ApiDraw {
    Handle mesh = 0;
    std::uint32_t surface = 0;
    Handle material = 0;
    std::uint64_t instanceId = 0; ///< RL-1.7
    scene::instances::Mat4f objectToWorld = scene::instances::identityMatrix();
    scene::CategoryFlags categories;
    bool viewModel = false;
    bool doubleSided = false;
    bool hidden = false;       ///< RL-3.4: the original is hidden by a mesh replacement
    bool meshReplaced = false;
    bool materialReplaced = false;
    std::string replacementMaterial; ///< the replacing material record ("" : none)
    std::uint32_t replacementParts = 0;
};

struct ApiFrame {
    fuse_relight_FrameRecord record{};
    std::vector<ApiDraw> draws;
    scene::instances::SceneFrameStats scene;
};

/// Remix category bits -> the RL-1.2 flags (IgnoreTransparencyLayer is deprecated and dropped; ViewModel is the
/// camera type, reported separately).
scene::CategoryFlags categoriesFromApiBits(std::uint32_t apiBits, bool* viewModel = nullptr);

/// Remix 3x4 (column vectors, translation in column 3) -> row-vector Mat4f.
scene::instances::Mat4f matrixFromRemixTransform(const float m[3][4]);

class ApiRuntime {
public:
    ApiRuntime();
    ~ApiRuntime();
    ApiRuntime(const ApiRuntime&) = delete;
    ApiRuntime& operator=(const ApiRuntime&) = delete;

    /// The process-wide instance (both C front ends).
    static ApiRuntime& global();

    /// Serializes a C entry point (everything below assumes the caller holds it).
    std::unique_lock<std::recursive_mutex> lock() { return std::unique_lock<std::recursive_mutex>(m_mutex); }

    // --- lifecycle ---------------------------------------------------------------------------------------------------
    /// `replacementMode` 0: none; 1: from the relight.replace.* options. `modDirectory`: an extra Remix mod ("": none).
    fuse_relight_Result initialize(std::uint32_t replacementMode, const std::string& modDirectory);
    /// Attaches an engine built by the caller (tests); replaces the current one.
    void attachReplacementEngine(std::unique_ptr<replace::ReplacementEngine> engine);
    fuse_relight_Result shutdown();
    bool initialized() const { return m_initialized; }

    // --- objects -----------------------------------------------------------------------------------------------------
    fuse_relight_Result createMaterial(std::uint64_t hash, mods::import::MaterialParams params, Handle& out);
    fuse_relight_Result destroyMaterial(Handle h);
    /// Takes the surfaces' vertex data (hashes and bounds are computed here).
    fuse_relight_Result createMesh(std::uint64_t hash, std::vector<ApiSurface> surfaces, Handle& out);
    fuse_relight_Result destroyMesh(Handle h);
    fuse_relight_Result createLight(std::uint64_t hash, const lk::RlLight& light, bool supported, Handle& out);
    fuse_relight_Result destroyLight(Handle h);

    // --- frame -------------------------------------------------------------------------------------------------------
    fuse_relight_Result setCamera(const ApiCamera& camera);
    fuse_relight_Result drawInstance(const ApiInstance& instance);
    fuse_relight_Result drawLight(Handle h);
    fuse_relight_Result setOption(const std::string& key, const std::string& value);
    fuse_relight_Result getOption(const std::string& key, std::string& value) const;
    fuse_relight_Result endFrame();
    /// Counts a call that failed validation (reported in the next record).
    void reject() { ++m_rejected; }

    // --- results -----------------------------------------------------------------------------------------------------
    bool hasFrame() const { return m_hasFrame; }
    const ApiFrame& lastFrame() const { return m_last; }
    const render::lights::RelightLightSet& lights() const { return m_lights; }
    const scene::instances::SceneModel& scene() const { return *m_scene; }
    replace::ReplacementEngine* replacement() { return m_engine.get(); }
    const ApiMesh* mesh(Handle h) const;
    const ApiMaterial* material(Handle h) const;
    const ApiLight* light(Handle h) const;

private:
    void resetObjects();

    std::recursive_mutex m_mutex;
    bool m_initialized = false;
    Handle m_nextHandle = 1;
    std::unordered_map<Handle, ApiMesh> m_meshes;
    std::unordered_map<Handle, ApiMaterial> m_materials;
    std::unordered_map<Handle, ApiLight> m_lights_;
    ApiMaterial m_defaultMaterial;
    // The frame being built.
    std::vector<ApiInstance> m_instances;
    std::vector<Handle> m_lightDraws;
    std::optional<ApiCamera> m_camera;
    std::uint32_t m_rejected = 0;
    std::uint32_t m_optionWrites = 0;
    // Systems.
    std::unique_ptr<scene::instances::SceneModel> m_scene;
    render::lights::RelightLightSet m_lights;
    std::unique_ptr<replace::ReplacementEngine> m_engine;
    std::unique_ptr<options::OptionLayerHandle> m_optionLayer;
    hash::HashRule m_assetRule;
    std::uint64_t m_frame = 0;
    bool m_hasFrame = false;
    ApiFrame m_last;
};

} // namespace fuse::relight::api
