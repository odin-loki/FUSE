// FUSE Relight RL-6.2: the runtime behind the C APIs (see api_runtime.hpp).
#include <fuse/relight/api/api_runtime.hpp>

#include <fuse/relight/capture/geometry/geometry_capture.hpp>
#include <fuse/relight/options/option_manager.hpp>
#include <fuse/relight/replace/replace_options.hpp>
#include <fuse/relight/replace/replacement_engine.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fuse::relight::api {

namespace inst = scene::instances;
using scene::InstanceCategories;

namespace {

/// The layer SetOption / SetConfigVariable write (above rtx.conf, baseGameMod and the environment; below the user and
/// quality layers).
constexpr const char* kOptionLayerName = "Relight API";
/// Remix API vertex (remixapi_HardcodedVertex) size: the vertex-layout hash component of every API surface, so both
/// front ends hash the same data identically.
constexpr std::uint64_t kApiVertexStride = 64;
constexpr std::uint32_t kVkIndexTypeUint32 = 1;
constexpr std::uint32_t kVkTopologyTriangleList = 3;

// Remix API category bit -> RL-1.2 category (remixapi_InstanceCategoryBit order; -1: no flag).
constexpr int kCategoryMap[] = {
    int(InstanceCategories::WorldUI),               // 0
    int(InstanceCategories::WorldMatte),            // 1
    int(InstanceCategories::Sky),                   // 2
    int(InstanceCategories::Ignore),                // 3
    int(InstanceCategories::IgnoreLights),          // 4
    int(InstanceCategories::IgnoreAntiCulling),     // 5
    int(InstanceCategories::IgnoreMotionBlur),      // 6
    int(InstanceCategories::IgnoreOpacityMicromap), // 7
    int(InstanceCategories::Hidden),                // 8
    int(InstanceCategories::Particle),              // 9
    int(InstanceCategories::Beam),                  // 10
    int(InstanceCategories::DecalStatic),           // 11
    int(InstanceCategories::DecalDynamic),          // 12
    int(InstanceCategories::DecalSingleOffset),     // 13
    int(InstanceCategories::DecalNoOffset),         // 14
    int(InstanceCategories::AlphaBlendToCutout),    // 15
    int(InstanceCategories::Terrain),               // 16
    int(InstanceCategories::AnimatedWater),         // 17
    int(InstanceCategories::ThirdPersonPlayerModel), // 18
    int(InstanceCategories::ThirdPersonPlayerBody), // 19
    int(InstanceCategories::IgnoreBakedLighting),   // 20
    int(InstanceCategories::IgnoreAlphaChannel),    // 21
    -1,                                             // 22 IgnoreTransparencyLayer (deprecated upstream, ignored)
    int(InstanceCategories::ParticleEmitter),       // 23
    int(InstanceCategories::SmoothNormals),         // 24
    int(InstanceCategories::HairCards),             // 25
};
constexpr std::uint32_t kViewModelBit = 1u << 26;

inst::MaterialType materialType(const ApiMaterial& m) {
    switch (m.params.surface) {
    case mods::import::SurfaceType::Translucent: return inst::MaterialType::Translucent;
    case mods::import::SurfaceType::Portal: return inst::MaterialType::RayPortal;
    default: return inst::MaterialType::Opaque;
    }
}

float param(const mods::import::MaterialParams& p, const char* name, int c = 0) {
    const auto it = p.values.find(name);
    return it == p.values.end() ? 0.f : it->second.value[std::size_t(c)];
}

replace::Mat4d toMat4d(const inst::Mat4f& m) {
    replace::Mat4d r{};
    for (std::size_t i = 0; i < 16; ++i) {
        r[i] = double(m[i]);
    }
    return r;
}

void hashMix(std::uint64_t& h, const void* data, std::size_t size) { h = hash::xxh64(data, size, h); }

/// The executable's name without extension (the replacement engine's default game id, as capture mode uses).
std::string exeStem() {
#ifdef _WIN32
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    const std::string stem = std::filesystem::path(std::string(path, n)).stem().string();
#else
    std::error_code ec;
    const std::string stem = std::filesystem::read_symlink("/proc/self/exe", ec).stem().string();
#endif
    return stem.empty() ? std::string("game") : stem;
}

ApiMaterial makeMaterial(std::uint64_t hash, mods::import::MaterialParams params) {
    ApiMaterial m;
    m.hash = hash;
    mods::import::sanitizeMaterialParams(params);
    m.params = std::move(params);
    m.bsdf = render::material::bsdfMaterialFromParams(m.params);
    m.emission = lk::float3(m.bsdf.emission.x, m.bsdf.emission.y, m.bsdf.emission.z);
    return m;
}

} // namespace

scene::CategoryFlags categoriesFromApiBits(std::uint32_t apiBits, bool* viewModel) {
    scene::CategoryFlags f;
    for (std::uint32_t bit = 0; bit < std::size(kCategoryMap); ++bit) {
        if ((apiBits >> bit) & 1u) {
            if (kCategoryMap[bit] >= 0) {
                f.set(InstanceCategories(kCategoryMap[bit]));
            }
        }
    }
    if (viewModel != nullptr) {
        *viewModel = (apiBits & kViewModelBit) != 0;
    }
    return f;
}

inst::Mat4f matrixFromRemixTransform(const float m[3][4]) {
    // Remix: column vectors, p' = M p with M 3x4 (translation in column 3). Row vectors: Mat4f = transpose.
    inst::Mat4f r = inst::identityMatrix();
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            r[std::size_t(col * 4 + row)] = m[row][col];
        }
    }
    return r;
}

ApiRuntime::ApiRuntime() : m_scene(std::make_unique<inst::SceneModel>()) {
    m_defaultMaterial = makeMaterial(0, mods::import::defaultMaterialParams(mods::import::SurfaceType::Opaque));
}

ApiRuntime::~ApiRuntime() = default;

ApiRuntime& ApiRuntime::global() {
    // Never destroyed: the DLL may be unloaded (or the process exit) while a foreign thread still holds the API.
    static ApiRuntime* runtime = new ApiRuntime();
    return *runtime;
}

fuse_relight_Result ApiRuntime::initialize(std::uint32_t replacementMode, const std::string& modDirectory) {
    if (replacementMode > 1) {
        return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
    }
    if (m_initialized) {
        shutdown();
    }
    m_assetRule = capture::geometry::GeometryCaptureConfig::fromOptions().assetRule;
    m_engine.reset();
    if (replacementMode == 1 || !modDirectory.empty()) {
        replace::registerReplaceOptions();
        replace::EngineConfig config;
        if (replacementMode == 1) {
            config = replace::EngineConfig::fromOptions(exeStem());
        } else {
            config.gameId = "api";
            config.hotReload = false;
        }
        if (!modDirectory.empty()) {
            config.mods.push_back({modDirectory, replace::ModKind::Remix});
        }
        bool anything = false;
        for (const std::vector<replace::ModRoot>* list : {&config.roots, &config.mods}) {
            for (const replace::ModRoot& r : *list) {
                std::error_code ec;
                anything = anything || std::filesystem::is_directory(std::filesystem::path(r.dir), ec);
            }
        }
        if (anything && (replacementMode == 0 || replace::ReplaceOptions::enable())) {
            m_engine = std::make_unique<replace::ReplacementEngine>(std::move(config));
        }
    }
    m_initialized = true;
    return FUSE_RELIGHT_SUCCESS;
}

void ApiRuntime::attachReplacementEngine(std::unique_ptr<replace::ReplacementEngine> engine) {
    m_engine = std::move(engine);
}

void ApiRuntime::resetObjects() {
    m_meshes.clear();
    m_materials.clear();
    m_lights_.clear();
    m_instances.clear();
    m_lightDraws.clear();
    m_camera.reset();
}

fuse_relight_Result ApiRuntime::shutdown() {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    resetObjects();
    m_engine.reset();
    if (m_optionLayer) {
        m_optionLayer->release(); // the layer's values leave every option
        m_optionLayer.reset();
        options::OptionManager::applyPendingValues(nullptr, false);
    }
    m_scene = std::make_unique<inst::SceneModel>();
    m_frame = 0;
    m_hasFrame = false;
    m_last = ApiFrame{};
    m_rejected = 0;
    m_optionWrites = 0;
    m_initialized = false;
    return FUSE_RELIGHT_SUCCESS;
}

// ---- objects --------------------------------------------------------------------------------------------------------

fuse_relight_Result ApiRuntime::createMaterial(std::uint64_t hash, mods::import::MaterialParams params, Handle& out) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    const Handle h = m_nextHandle++;
    m_materials.emplace(h, makeMaterial(hash, std::move(params)));
    out = h;
    return FUSE_RELIGHT_SUCCESS;
}

fuse_relight_Result ApiRuntime::destroyMaterial(Handle h) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    return m_materials.erase(h) != 0 ? FUSE_RELIGHT_SUCCESS : FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE;
}

fuse_relight_Result ApiRuntime::createMesh(std::uint64_t meshHash, std::vector<ApiSurface> surfaces, Handle& out) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    if (surfaces.empty()) {
        return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
    }
    for (ApiSurface& s : surfaces) {
        const std::size_t vertexCount = s.positions.size() / 3;
        if (vertexCount == 0 || s.positions.size() % 3 != 0 || vertexCount > 0xFFFFFFFFu) {
            return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
        }
        if ((!s.normals.empty() && s.normals.size() != vertexCount * 3) ||
            (!s.texcoords.empty() && s.texcoords.size() != vertexCount * 2)) {
            return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
        }
        if (s.indices.empty()) {
            s.indices.resize(vertexCount - vertexCount % 3);
            for (std::size_t i = 0; i < s.indices.size(); ++i) {
                s.indices[i] = std::uint32_t(i);
            }
        }
        if (s.indices.empty() || s.indices.size() % 3 != 0 || s.indices.size() > 0xFFFFFFFFu) {
            return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
        }
        for (std::uint32_t i : s.indices) {
            if (i >= vertexCount) {
                return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
            }
        }
        if (s.material != 0 && m_materials.find(s.material) == m_materials.end()) {
            return FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE;
        }
        // RL-1.3 components (the ones an API surface has; legacy and vertex-shader components stay 0).
        const auto unique = hash::sortedUniqueIndices(s.indices.data(), std::uint32_t(s.indices.size()), 4,
                                                      std::uint32_t(vertexCount - 1));
        using HC = hash::HashComponent;
        s.hashes = hash::GeometryHashes{};
        s.hashes[HC::Positions] = hash::hashVertexRegion(reinterpret_cast<const std::uint8_t*>(s.positions.data()),
                                                         s.positions.size() * sizeof(float), 12, 12, unique);
        if (!s.texcoords.empty()) {
            s.hashes[HC::Texcoords] = hash::hashVertexRegion(reinterpret_cast<const std::uint8_t*>(s.texcoords.data()),
                                                             s.texcoords.size() * sizeof(float), 8, 8, unique);
        }
        s.hashes[HC::Indices] = hash::hashContiguousMemory(s.indices.data(), s.indices.size() * sizeof(std::uint32_t));
        s.hashes[HC::GeometryDescriptor] = hash::hashGeometryDescriptor(
            std::uint32_t(s.indices.size()), std::uint32_t(vertexCount), kVkIndexTypeUint32, kVkTopologyTriangleList);
        s.hashes[HC::VertexLayout] = hash::hashVertexLayoutStride(kApiVertexStride);
        s.assetHash = s.hashes.hashForRule(m_assetRule);
        s.bounds = inst::AxisAlignedBoundingBox{};
        for (std::size_t v = 0; v < vertexCount; ++v) {
            const inst::Vec3 p{s.positions[v * 3], s.positions[v * 3 + 1], s.positions[v * 3 + 2]};
            inst::AxisAlignedBoundingBox b;
            b.minPos = p;
            b.maxPos = p;
            s.bounds.unionWith(b);
        }
    }
    const Handle h = m_nextHandle++;
    m_meshes.emplace(h, ApiMesh{meshHash, std::move(surfaces)});
    out = h;
    return FUSE_RELIGHT_SUCCESS;
}

fuse_relight_Result ApiRuntime::destroyMesh(Handle h) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    return m_meshes.erase(h) != 0 ? FUSE_RELIGHT_SUCCESS : FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE;
}

fuse_relight_Result ApiRuntime::createLight(std::uint64_t hash, const lk::RlLight& light, bool supported, Handle& out) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    if (supported && light.kind == lk::kRlKindNone) {
        return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
    }
    const Handle h = m_nextHandle++;
    m_lights_.emplace(h, ApiLight{hash, light, supported});
    out = h;
    return FUSE_RELIGHT_SUCCESS;
}

fuse_relight_Result ApiRuntime::destroyLight(Handle h) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    return m_lights_.erase(h) != 0 ? FUSE_RELIGHT_SUCCESS : FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE;
}

const ApiMesh* ApiRuntime::mesh(Handle h) const {
    const auto it = m_meshes.find(h);
    return it == m_meshes.end() ? nullptr : &it->second;
}
const ApiMaterial* ApiRuntime::material(Handle h) const {
    const auto it = m_materials.find(h);
    return it == m_materials.end() ? nullptr : &it->second;
}
const ApiLight* ApiRuntime::light(Handle h) const {
    const auto it = m_lights_.find(h);
    return it == m_lights_.end() ? nullptr : &it->second;
}

// ---- frame ----------------------------------------------------------------------------------------------------------

fuse_relight_Result ApiRuntime::setCamera(const ApiCamera& camera) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    if (camera.type > 2) {
        return FUSE_RELIGHT_ERROR_INVALID_ARGUMENT;
    }
    if (camera.type == 0) {
        m_camera = camera; // the main camera drives anti-culling; sky / view-model cameras are accepted and not used
    }
    return FUSE_RELIGHT_SUCCESS;
}

fuse_relight_Result ApiRuntime::drawInstance(const ApiInstance& instance) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    if (m_meshes.find(instance.mesh) == m_meshes.end()) {
        return FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE;
    }
    m_instances.push_back(instance);
    return FUSE_RELIGHT_SUCCESS;
}

fuse_relight_Result ApiRuntime::drawLight(Handle h) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    if (m_lights_.find(h) == m_lights_.end()) {
        return FUSE_RELIGHT_ERROR_UNKNOWN_HANDLE;
    }
    m_lightDraws.push_back(h);
    return FUSE_RELIGHT_SUCCESS;
}

fuse_relight_Result ApiRuntime::setOption(const std::string& key, const std::string& value) {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    options::OptionBase* option = options::OptionManager::findOption(key);
    if (option == nullptr) {
        return FUSE_RELIGHT_ERROR_UNKNOWN_OPTION;
    }
    if (!m_optionLayer) {
        m_optionLayer = std::make_unique<options::OptionLayerHandle>(options::OptionManager::acquireLayer(
            "", options::OptionLayerKey(options::kMaxDynamicLayerPriority, kOptionLayerName)));
    }
    if (!*m_optionLayer) {
        return FUSE_RELIGHT_ERROR_GENERAL;
    }
    options::OptionConfig config;
    config.set(option->getFullName(), value);
    option->readOption(config, m_optionLayer->get());
    ++m_optionWrites;
    return FUSE_RELIGHT_SUCCESS;
}

fuse_relight_Result ApiRuntime::getOption(const std::string& key, std::string& value) const {
    const options::OptionBase* option = options::OptionManager::findOption(key);
    if (option == nullptr) {
        return FUSE_RELIGHT_ERROR_UNKNOWN_OPTION;
    }
    value = option->getResolvedValueAsString();
    return FUSE_RELIGHT_SUCCESS;
}

fuse_relight_Result ApiRuntime::endFrame() {
    if (!m_initialized) {
        return FUSE_RELIGHT_ERROR_NOT_INITIALIZED;
    }
    const std::uint32_t optionWrites = m_optionWrites;
    if (m_optionWrites != 0) {
        options::OptionManager::applyPendingValues(nullptr, false);
        m_optionWrites = 0;
    }
    ApiFrame frame;
    fuse_relight_FrameRecord& rec = frame.record;
    rec.structSize = sizeof(fuse_relight_FrameRecord);
    rec.frame = m_frame;
    rec.optionWrites = optionWrites;
    rec.replacementActive = m_engine ? 1u : 0u;

    if (m_engine) {
        m_engine->beginFrame(m_frame);
    }
    m_lights.beginFrame();
    std::uint64_t sceneDigest = 0;
    std::uint32_t drawIndex = 0;
    for (const ApiInstance& instance : m_instances) {
        const ApiMesh* mesh = this->mesh(instance.mesh);
        if (mesh == nullptr) {
            continue; // destroyed after DrawInstance: dropped, as upstream drops a stale handle
        }
        ++rec.instancesDrawn;
        bool viewModel = false;
        const scene::CategoryFlags categories = categoriesFromApiBits(instance.apiCategories, &viewModel);
        for (std::uint32_t si = 0; si < mesh->surfaces.size(); ++si) {
            const ApiSurface& s = mesh->surfaces[si];
            const ApiMaterial* mat = s.material != 0 ? material(s.material) : &m_defaultMaterial;
            if (mat == nullptr) {
                mat = &m_defaultMaterial; // the surface's material was destroyed: default opaque
            }
            inst::SceneDrawInput in;
            in.drawCallId = drawIndex;
            in.geometry = s.hashes;
            in.boundingBox = s.bounds;
            in.assetHash = s.assetHash;
            in.materialHash = mat->hash;
            const std::uint64_t identity[2] = {mat->hash, s.material};
            in.materialIdentityHash = hash::xxh64(identity, sizeof(identity), 0);
            in.objectToWorld = instance.objectToWorld;
            in.categories = categories;
            in.cameraType = viewModel ? scene::CameraType::ViewModel : scene::CameraType::Main;
            in.materialType = materialType(*mat);
            in.subsurface.measurementDistance = param(mat->params, "subsurface_measurement_distance");
            in.subsurface.diffusionProfile = param(mat->params, "subsurface_diffusion_profile") != 0.f;
            const inst::SceneDrawResult res = m_scene->submitDraw(in);

            ApiDraw d;
            d.mesh = instance.mesh;
            d.surface = si;
            d.material = s.material;
            d.instanceId = res.instanceId;
            d.objectToWorld = res.objectToWorld;
            d.categories = categories;
            d.viewModel = viewModel;
            d.doubleSided = instance.doubleSided;
            if (m_engine) {
                replace::DrawInput di;
                di.index = drawIndex;
                di.geometryValid = true;
                di.hashes = s.hashes;
                di.materialHash = mat->hash;
                di.categories = categories;
                di.objectToWorld = toMat4d(instance.objectToWorld);
                di.instanceId = res.instanceId;
                const replace::ReplacedDraw rd = m_engine->replaceDraw(di);
                d.meshReplaced = rd.meshReplaced;
                d.hidden = !rd.drawOriginal;
                d.materialReplaced = rd.materialReplaced;
                d.replacementMaterial = rd.materialRecord;
                d.replacementParts = std::uint32_t(rd.parts.size());
                rec.meshReplaced += rd.meshReplaced ? 1u : 0u;
                rec.materialReplaced += rd.materialReplaced ? 1u : 0u;
                rec.hiddenDraws += rd.drawOriginal ? 0u : 1u;
                rec.replacementParts += std::uint32_t(rd.parts.size());
            }
            const lk::float3 e = mat->emission;
            if (!d.hidden && (e.x > 0.f || e.y > 0.f || e.z > 0.f)) {
                render::lights::EmissiveMesh em;
                em.positions = s.positions.data();
                em.stride = 12;
                em.vertexCount = std::uint32_t(s.positions.size() / 3);
                em.indices = s.indices.data();
                em.indexCount = std::uint32_t(s.indices.size());
                em.index32 = true;
                em.objectToWorld = instance.objectToWorld.data();
                em.radiance = e;
                em.twoSided = instance.doubleSided;
                em.key = res.instanceId;
                rec.emissiveTriangles += m_lights.addEmissiveTriangles(em);
            }
            const std::uint64_t digest[4] = {res.instanceId, mesh->hash, mat->hash, d.hidden ? 1u : 0u};
            hashMix(sceneDigest, digest, sizeof(digest));
            hashMix(sceneDigest, res.objectToWorld.data(), sizeof(float) * 16);
            ++rec.surfacesDrawn;
            ++drawIndex;
            frame.draws.push_back(std::move(d));
        }
    }
    for (Handle h : m_lightDraws) {
        const ApiLight* l = light(h);
        if (l != nullptr && l->supported && m_lights.addLight(l->light, l->hash)) {
            ++rec.authoredLights;
        }
    }
    if (m_engine) {
        static const std::vector<scene::LightRecord> kNoGameLights;
        (void)m_engine->endFrame(kNoGameLights);
    }
    m_lights.build();
    rec.lights = m_lights.lightCount();
    std::uint64_t lightDigest = 0;
    if (!m_lights.table().empty()) {
        hashMix(lightDigest, m_lights.table().data(), m_lights.table().size() * sizeof(lk::float4));
    }

    std::optional<scene::CameraState> camera;
    if (m_camera) {
        scene::CameraState c;
        c.type = scene::CameraType::Main;
        c.frameLastTouched = m_scene->currentFrame();
        std::copy(m_camera->view.begin(), m_camera->view.end(), c.worldToView.begin());
        std::copy(m_camera->projection.begin(), m_camera->projection.end(), c.viewToProjection.begin());
        camera = c;
        rec.cameraValid = 1;
    }
    frame.scene = m_scene->endFrame(camera ? &*camera : nullptr);
    rec.sceneInstances = frame.scene.activeInstances;
    rec.createdInstances = frame.scene.createdInstances;
    rec.liveMeshes = std::uint32_t(m_meshes.size());
    rec.liveMaterials = std::uint32_t(m_materials.size());
    rec.liveLights = std::uint32_t(m_lights_.size());
    rec.rejectedCalls = m_rejected;
    rec.sceneDigest = sceneDigest;
    rec.lightDigest = lightDigest;

    m_last = std::move(frame);
    m_hasFrame = true;
    m_instances.clear();
    m_lightDraws.clear();
    m_camera.reset();
    m_rejected = 0;
    ++m_frame;
    return FUSE_RELIGHT_SUCCESS;
}

} // namespace fuse::relight::api
