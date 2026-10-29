#include <fuse/editor/terrain_builder.hpp>
#include <fuse/ecs/components/terrain.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/collider.hpp>
#include <fuse/ecs/components/environment.hpp>
#include <fuse/ecs/components/light.hpp>
#include <atomic>
#include <cstdlib>
#include <fuse/editor/runtime_viewport.hpp>

#include <fuse/core/track_b.hpp>

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/viewport_present_gate.hpp>
#include <fuse/editor/viewport_swapchain_recreate.hpp>
#include <fuse/editor/viewport_swapchain_wiring.hpp>
#include <fuse/editor/viewport_vulkan_surface.hpp>
#include <fuse/ecs/components/mesh.hpp>
#include <fuse/ecs/components/sdf_object.hpp>
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/ecs/spawn_query.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/window_wsi.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/hybrid/project_flags.hpp>
#include <fuse/cook/texture_cook.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/project/t3d_asset_vfs.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>
#include <fuse/scene/project_io.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_stub.hpp>
#include <fuse/scene/wire_runtime_bind.hpp>

#include <vector>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/mesh_sdf_preview_stub.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
#include <fuse/hybrid/hybrid_scene_renderer.hpp>
#include <fuse/renderer/geometry/meshlet_builder.hpp>
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/render_command_list.hpp>
#include <fuse/renderer/vk/present_path.hpp>
#endif

namespace fuse::editor {

namespace {

struct EulerDeg {
    float yaw = 0.f;
    float pitch = 0.f;
    float roll = 0.f;
};

EulerDeg quatToEulerDeg(const ecs::quat& q) {
    constexpr float kRadToDeg = 57.2957795f;

    const float sinrCosp = 2.f * (q.w * q.x + q.y * q.z);
    const float cosrCosp = 1.f - 2.f * (q.x * q.x + q.y * q.y);
    const float roll = std::atan2(sinrCosp, cosrCosp) * kRadToDeg;

    const float sinp = 2.f * (q.w * q.y - q.z * q.x);
    float pitch = 0.f;
    if (std::abs(sinp) >= 1.f) {
        pitch = std::copysign(90.f, sinp);
    } else {
        pitch = std::asin(sinp) * kRadToDeg;
    }

    const float sinyCosp = 2.f * (q.w * q.z + q.x * q.y);
    const float cosyCosp = 1.f - 2.f * (q.y * q.y + q.z * q.z);
    const float yaw = std::atan2(sinyCosp, cosyCosp) * kRadToDeg;

    return {yaw, pitch, roll};
}

[[maybe_unused]] void applyEcsTransformToSceneObject3D(const ecs::Transform& transform, fuse::SceneObject3D& object) {
    object.setPosition(transform.position.x, transform.position.y);
    object.setZ(transform.position.z);
    const EulerDeg euler = quatToEulerDeg(transform.rotation);
    object.setYawDeg(euler.yaw);
    object.setPitchDeg(euler.pitch);
    object.setRollDeg(euler.roll);
}

} // namespace

#if defined(FUSE_VULKAN_BACKEND)
struct WindowPrewarmToken {
    WindowPresentRequest request;
    u32 width = 640;
    u32 height = 480;
    std::unique_ptr<fuse::hybrid::HybridRendererBootstrap> hybrid;
};

struct RuntimeViewportHeadlessGpuStub {
    std::unique_ptr<fuse::renderer::RhiContext> context;
    std::unique_ptr<fuse::hybrid::HybridRendererBootstrap> hybrid;
    std::unique_ptr<fuse::renderer::PresentPath> fallbackPresentPath;
    fuse::world2d::World2D embedWorld2D;
    fuse::world3d::World3D embedWorld3D;
    std::vector<std::unique_ptr<fuse::SceneObject3D>> ecsMirrorObjects;
    u32 submittedFrames = 0;
    bool externalSwapchainWired = false;
    bool worldsAttached = false;
    /// The instance keeps `const char*` into the request's extension names: keep them alive.
    std::shared_ptr<WindowPrewarmToken> instanceRequest;
    bool windowBringUpDone = false; ///< tickWindowPresent_ step 1 finished (instance published)
    const fuse::hybrid::HybridSceneRenderer* textureLoaderOn = nullptr; ///< GPU scene that has the texture loader
};

RuntimeViewportHeadlessGpuStub* asHeadlessGpuStub(void* stub) {
    return static_cast<RuntimeViewportHeadlessGpuStub*>(stub);
}

const RuntimeViewportHeadlessGpuStub* asHeadlessGpuStub(const void* stub) {
    return static_cast<const RuntimeViewportHeadlessGpuStub*>(stub);
}

void recordViewportPresentDiagnostics(RuntimeEmbedSession& session,
                                      const ViewportSwapchainPresentResult& present,
                                      const ViewportSwapchainHandoff& handoff,
                                      const fuse::renderer::PresentPath& presentPath) {
    if (!present.attempted) {
        return;
    }

    ++session.consumedSwapchainPresentTicks;
    if (present.presented) {
        ++session.swapchainPresentAfterRecreateCount;
    }
    if (present.viewportQtPresentPathReady) {
        ++session.qtPresentPathReadyTicks;
    }
    if (present.viewportQtPresentPathEligible) {
        ++session.qtPresentPathEligibleTicks;
    }
    if (present.qtPresentGateEnabled && viewportQtPresentEligible(handoff)) {
        ++session.qtPresentEligibleTicks;
    }
    const fuse::renderer::PresentPathStatus& status = presentPath.status();
    if (status.realPresentCallCount > session.realPresentCallCount) {
        session.realPresentCallCount = status.realPresentCallCount;
    }
    if (status.qtRealPresentCallCount > session.qtRealPresentCallCount) {
        session.qtRealPresentCallCount = status.qtRealPresentCallCount;
    }
    if (status.presentSkippedNoWsiCount > session.presentSkippedNoWsiCount) {
        session.presentSkippedNoWsiCount = status.presentSkippedNoWsiCount;
    }
}
#endif

bool loadMaterialTextureRgba8(const std::string& path, const std::string& projectRoot, u32& width, u32& height,
                              std::vector<u32>& texels) {
    std::string resolved = path;
    if (!projectRoot.empty() && !path.empty() && path.front() != '/' && !std::ifstream(path)) {
        resolved = projectRoot + "/" + path;
    }
    fuse::cook::TextureSource src;
    if (!fuse::cook::load_texture_source(resolved, src, nullptr, nullptr, false) || src.width == 0u ||
        src.height == 0u || src.rgba8.size() < static_cast<usize>(src.width) * src.height * 4u) {
        return false;
    }
    width = src.width;
    height = src.height;
    texels.resize(static_cast<usize>(width) * height);
    for (usize i = 0; i < texels.size(); ++i) {
        const u8* p = &src.rgba8[i * 4u];
        texels[i] = u32{p[0]} | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
    }
    return true;
}

namespace {
std::atomic<int> g_previewSun{-1}; ///< -1: not set yet (FUSE_EDITOR_NO_PREVIEW_SUN decides)
} // namespace

void setEditorPreviewSun(bool on) { g_previewSun.store(on ? 1 : 0); }

namespace {
/// FUSE_EDITOR_VIEW_MODE=0..3 picks the startup view mode (screenshots / CI).
int initialViewMode() {
    const char* v = std::getenv("FUSE_EDITOR_VIEW_MODE");
    const int m = (v != nullptr) ? std::atoi(v) : 0;
    return (m >= 0 && m <= 3) ? m : 0;
}
std::atomic<int> g_viewMode{initialViewMode()};
} // namespace

void setEditorViewMode(EditorViewMode mode) { g_viewMode.store(static_cast<int>(mode)); }
EditorViewMode editorViewMode() { return static_cast<EditorViewMode>(g_viewMode.load()); }

bool editorPreviewSun() {
    const int v = g_previewSun.load();
    if (v >= 0) return v != 0;
    return std::getenv("FUSE_EDITOR_NO_PREVIEW_SUN") == nullptr;
}

#if defined(FUSE_VULKAN_BACKEND)
/// Material Editor textures: image files (PNG / TGA / BMP / JPEG), relative paths against the project root. The GPU
/// scene starts lazily (first frame), so this runs before every frame until it found one.
void ensureMaterialTextureLoader(RuntimeViewportHeadlessGpuStub* gpu, const RuntimeEmbedSession& session) {
    if (gpu == nullptr || gpu->hybrid == nullptr) {
        return;
    }
    fuse::hybrid::HybridSceneRenderer* scene = gpu->hybrid->composer().gpuScene();
    if (scene != nullptr) {
        scene->setFallbackSun(editorPreviewSun());
        scene->setUnlit(editorViewMode() == EditorViewMode::Unlit || editorViewMode() == EditorViewMode::Wireframe);
    }
    if (scene == nullptr || scene == gpu->textureLoaderOn) {
        return;
    }
    // The session outlives the hybrid (both belong to the hook); the root is read when a texture first loads.
    const RuntimeEmbedSession* live = &session;
    scene->setTextureLoader([live](const std::string& path, u32& w, u32& h, std::vector<u32>& texels) {
        return loadMaterialTextureRgba8(path, live->projectRoot, w, h, texels);
    });
    gpu->textureLoaderOn = scene;
}

void attachEmbedWorldsToHybrid(RuntimeViewportHeadlessGpuStub* gpu, RuntimeEmbedSession& session,
                               const fuse::project::ProjectManifest& manifest) {
    if (gpu == nullptr || gpu->hybrid == nullptr) {
        return;
    }
    if (gpu->worldsAttached) {
        // Attached before a project loaded: apply the project's dimensions now.
        if (session.worldLoaded) {
            const fuse::hybrid::DimensionFlags flags = fuse::project::toDimensionFlags(manifest.dimensions);
            gpu->hybrid->composer().setProjectFlags(flags);
            gpu->embedWorld3D.setEnabled(flags.enable3D);
            gpu->embedWorld2D.setEnabled(flags.enable2D);
        }
        return;
    }

    fuse::hybrid::DimensionFlags flags = fuse::project::toDimensionFlags(manifest.dimensions);
    if (!session.worldLoaded) {
        // No project: the editor scene is a pure 3D scene. The composer's UI flag would draw its white placeholder
        // UI marker quad (top centre) over the viewport, and there is no 2D world to show.
        flags.enable3D = true;
        flags.enable2D = false;
        flags.enableUI = false;
    }
    gpu->hybrid->composer().setProjectFlags(flags);
    gpu->embedWorld3D.setClearColor(0.08f, 0.12f, 0.18f);
    gpu->embedWorld3D.setEnabled(flags.enable3D);
    gpu->embedWorld2D.setEnabled(flags.enable2D);
    gpu->hybrid->composer().attachWorld2D(&gpu->embedWorld2D);
    gpu->hybrid->composer().attachWorld3D(&gpu->embedWorld3D);
    gpu->worldsAttached = true;
    ++session.hybridComposerFrames;
}

void maybeRetireSoftwarePlaceholder(RuntimeViewportHeadlessGpuStub* gpu, RuntimeEmbedSession& session,
                                    const ViewportSwapchainHandoff& handoff) {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (gpu == nullptr || gpu->hybrid == nullptr) {
        return;
    }

    if (!shouldDisableSoftwarePlaceholderForEmbed(handoff, gpu->externalSwapchainWired)) {
        return;
    }

    if (gpu->hybrid->composer().softwarePlaceholderEnabled()) {
        gpu->hybrid->composer().setSoftwarePlaceholderEnabled(false);
        ++session.softwarePlaceholderRetiredTicks;
    }
#endif
}
#endif

RuntimeViewportHook::~RuntimeViewportHook() {
    if (m_windowPresentState.load(std::memory_order_acquire) == WindowPresentState::SurfaceWired) {
        fuse::core::setTrackBHostFeature(fuse::core::TrackBHostFeature::EditorViewportPresent, false);
    }
#if defined(FUSE_VULKAN_BACKEND)
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu != nullptr) {
        if (gpu->hybrid != nullptr) {
            gpu->hybrid->shutdown();
            gpu->hybrid.reset();
        }
        if (gpu->context != nullptr) {
            drainViewportGpuContext(*gpu->context);
        }
        delete gpu;
    }
    m_headlessGpuStub = nullptr;
#endif
}

void RuntimeViewportHook::requestResize(u32 width, u32 height) {
    std::lock_guard<std::mutex> lock(m_resizeMutex);
    if (m_hasPendingResize) {
        if (width > 0) {
            m_pendingWidth = width;
        }
        if (height > 0) {
            m_pendingHeight = height;
        }
    } else {
        m_pendingWidth = width;
        m_pendingHeight = height;
        m_hasPendingResize = true;
    }
}

void RuntimeViewportHook::setProjectLabel(std::string label) {
    m_lastProjectLabel = std::move(label);
    m_panel.setProjectLabel(m_lastProjectLabel);
}

void RuntimeViewportHook::setProjectRoot(std::string root) {
    if (m_embedSession.projectRoot == root) {
        return;
    }

    m_embedSession.reset();
    m_embedSession.projectRoot = std::move(root);
    m_embedded = false;
    m_surfaceHandoff = {};
    m_materialCookCache.clear();
    m_materialLoadsPending = false;
    m_shaderCookCache.clear();
    m_shaderLoadsPending = false;
}

void RuntimeViewportHook::setExternalSurfaceHandle(void* vkSurface, u32 width, u32 height,
                                                   const char* handoffSource, bool qtStubSurface,
                                                   bool qtRealSurface, void* qtVkInstance) {
    m_surfaceHandoff.nativeSurface = vkSurface;
    m_surfaceHandoff.width = width > 0 ? width : m_panel.width();
    m_surfaceHandoff.height = height > 0 ? height : m_panel.height();
    m_surfaceHandoff.pending = vkSurface != nullptr;
    m_surfaceHandoff.consumed = false;
    m_surfaceHandoff.qtStubSurface = qtStubSurface;
    m_surfaceHandoff.qtRealSurface = qtRealSurface;
    m_surfaceHandoff.qtVkInstance = qtVkInstance;
    m_surfaceHandoff.handoffSource = handoffSource;

#if defined(FUSE_VULKAN_BACKEND)
    m_surfaceHandoff.surface.kind = fuse::renderer::SurfaceKind::External;
    m_surfaceHandoff.surface.nativeSurface = vkSurface;
    m_surfaceHandoff.swapchainDesc.surface = m_surfaceHandoff.surface;
    m_surfaceHandoff.swapchainDesc.width = m_surfaceHandoff.width;
    m_surfaceHandoff.swapchainDesc.height = m_surfaceHandoff.height;
#endif

    m_embedSession.surfaceHandoffPending = m_surfaceHandoff.pending;
    m_embedSession.surfaceHandoffConsumed = false;
    if (m_surfaceHandoff.pending) {
        ++m_embedSession.surfaceHandoffCount;
    }
}

#if defined(FUSE_VULKAN_BACKEND)
fuse::renderer::SwapchainDesc RuntimeViewportHook::buildSwapchainDescHandoff() const {
    fuse::renderer::SwapchainDesc desc = m_surfaceHandoff.swapchainDesc;
    if (desc.width == 0) {
        desc.width = m_panel.width();
    }
    if (desc.height == 0) {
        desc.height = m_panel.height();
    }
    if (desc.surface.kind != fuse::renderer::SurfaceKind::External) {
        desc.surface.kind = fuse::renderer::SurfaceKind::Headless;
    }
    return desc;
}
#endif

void RuntimeViewportHook::applyPendingResize_() {
    u32 width = 0;
    u32 height = 0;
    bool pending = false;

    {
        std::lock_guard<std::mutex> lock(m_resizeMutex);
        if (m_hasPendingResize) {
            width = m_pendingWidth;
            height = m_pendingHeight;
            pending = true;
            m_hasPendingResize = false;
        }
    }

    if (pending) {
        const u32 appliedWidth = width > 0 ? width : m_panel.width();
        const u32 appliedHeight = height > 0 ? height : m_panel.height();
        m_panel.setDimensions(appliedWidth, appliedHeight);

#if defined(FUSE_VULKAN_BACKEND)
        RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
        if (gpu != nullptr && windowPresentActive_()) {
            // Presenting viewport: rebuild the swapchain on the UI surface before the next frame.
            // No stand-alone present cycle here — every presented image is a rendered frame.
            fuse::renderer::PresentPath* presentPath =
                gpu->hybrid != nullptr ? gpu->hybrid->presentPath() : nullptr;
            if (presentPath != nullptr && gpu->externalSwapchainWired) {
                presentPath->requestResize(appliedWidth, appliedHeight);
                if (presentPath->hasPendingResize()) {
                    ++m_embedSession.swapchainRecreateAttempts;
                    // Nothing may still reference the old images / render pass / framebuffers.
                    drainViewportGpuContext(*gpu->hybrid->rendererBootstrap().rhiContext());
                    const u32 before = presentPath->status().swapchainRecreateCount;
                    if (presentPath->recreateSwapchain() &&
                        presentPath->status().swapchainRecreateCount > before) {
                        ++m_embedSession.swapchainRecreateCount;
                        m_wpRecreates.fetch_add(1, std::memory_order_acq_rel);
                    }
                }
                if (const fuse::renderer::VulkanSwapchain* swapchain =
                        gpu->hybrid->rendererBootstrap().rhiContext()->bootstrap().swapchain()) {
                    m_wpWidth.store(swapchain->info().width, std::memory_order_release);
                    m_wpHeight.store(swapchain->info().height, std::memory_order_release);
                }
            }
        } else if (gpu != nullptr) {
            const bool handoffConsumed = m_surfaceHandoff.consumed || gpu->externalSwapchainWired;
            fuse::renderer::PresentPath* presentPath =
                gpu->hybrid != nullptr ? gpu->hybrid->presentPath() : nullptr;
            if (presentPath != nullptr) {
                presentPath->requestResize(appliedWidth, appliedHeight);
                if (presentPath->hasPendingResize()) {
                    ++m_embedSession.swapchainRecreateAttempts;
                }
                if (presentPath->recreateSwapchain()) {
                    ++m_embedSession.swapchainRecreateCount;
                }
                if (handoffConsumed) {
                    const ViewportSwapchainPresentResult present =
                        presentViewportSwapchainFrame(*presentPath, &m_surfaceHandoff);
                    recordViewportPresentDiagnostics(m_embedSession, present, m_surfaceHandoff,
                                                     *presentPath);
                    maybeRetireSoftwarePlaceholder(gpu, m_embedSession, m_surfaceHandoff);
                }
            } else if (gpu->context != nullptr) {
                const ViewportSwapchainRecreateResult queued = requestViewportSwapchainRecreate(
                    *gpu->context, gpu->fallbackPresentPath, appliedWidth, appliedHeight);
                if (queued.attempted) {
                    ++m_embedSession.swapchainRecreateAttempts;
                }

                ViewportSwapchainPresentResult present{};
                const ViewportSwapchainRecreateResult applied =
                    applyViewportPendingSwapchainRecreateAndPresent(*gpu->context,
                                                                    gpu->fallbackPresentPath,
                                                                    handoffConsumed, &present);
                if (applied.recreated) {
                    ++m_embedSession.swapchainRecreateCount;
                }
                if (present.attempted && gpu->fallbackPresentPath != nullptr) {
                    recordViewportPresentDiagnostics(m_embedSession, present, m_surfaceHandoff,
                                                     *gpu->fallbackPresentPath);
                    maybeRetireSoftwarePlaceholder(gpu, m_embedSession, m_surfaceHandoff);
                }
            }
        }
#endif
    }
}

void RuntimeViewportHook::ensureWorldLoaded_(EditorHost& host) {
    if (m_embedSession.worldLoaded || m_embedSession.projectRoot.empty()) {
        return;
    }

    const fuse::project::LoadResult projectLoad =
        fuse::project::loadFromDirectory(m_embedSession.projectRoot);
    if (projectLoad.status != fuse::project::LoadStatus::Ok) {
        fuse::log::warn("RuntimeViewportHook: unable to load project from %s",
                        m_embedSession.projectRoot.c_str());
        return;
    }

    fuse::jobs::setProjectWorkerCap(projectLoad.manifest.workerCap);

    const fuse::project::ProjectVfsMountResult vfsMount =
        fuse::project::mountProjectAssetRoots(projectLoad.manifest);
    m_embedSession.projectVfsMounts = vfsMount.mountsAdded;

    const fuse::project::Ensure3DWorldResult prepared =
        fuse::project::ensureDefault3DWorldReady(projectLoad);
    if (!prepared.ok && !projectLoad.manifest.defaultWorld3D.empty()) {
        fuse::log::warn("RuntimeViewportHook: 3D world prepare failed: %s", prepared.note.c_str());
        return;
    }

    fuse::scene::Scene& runtimeScene = host.runtimeScene();
    const fuse::scene::SerialiseResult loaded =
        fuse::scene::loadForProject(runtimeScene, projectLoad);
    if (loaded.status != fuse::scene::SerialiseStatus::Ok) {
        fuse::log::warn("RuntimeViewportHook: world load failed: %s", loaded.error.c_str());
        return;
    }

    m_embedSession.loadedWorldPath = fuse::scene::resolveDefaultWorldPath(projectLoad.manifest);
    m_embedSession.worldEntityCount = runtimeScene.entityCount();
    ecs::Registry& editorRegistry = host.editorScene().registry();
    std::vector<ecs::EntityID> entitiesBefore;
    editorRegistry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform&) { entitiesBefore.push_back(id); });
    const fuse::scene::WireRuntimeBindResult wireBindings =
        fuse::scene::applyWireBindingsFromScene(editorRegistry, runtimeScene);
    importLevelMeshBounds_(editorRegistry, runtimeScene, entitiesBefore, m_embedSession.loadedWorldPath);
    m_embedSession.wireDatablockEntries = wireBindings.datablockEntries;
    m_embedSession.wireMaterialEntries = wireBindings.materialEntries;
    m_embedSession.wireEcsMaterialApplied = wireBindings.ecsMaterialApplied;
    m_embedSession.wireEcsSpawnApplied = wireBindings.ecsSpawnApplied;

    const fuse::project::T3DDatablockResolveResult materialBindings =
        fuse::project::resolveT3DBindingsFromScene(runtimeScene);
    const fuse::project::T3DMaterialVfsResolveResult materialVfs =
        fuse::project::resolveT3DMaterialVfsFromBindings(materialBindings);
    m_embedSession.materialVfsResolved = materialVfs.resolvedCount;
    m_embedSession.materialVfsUnresolved = materialVfs.unresolvedCount;
    m_embedSession.vfsAssetPathsRemapped = fuse::project::countRemappedAssetVfsPaths(materialBindings);

    const fuse::project::T3DMaterialVfsAsyncLoadResult materialLoads =
        fuse::project::submitT3DMaterialLoadsAsync(materialBindings, &m_materialCookCache);
    m_embedSession.materialCookCacheHits = materialLoads.cookCacheHits;
    m_embedSession.materialAsyncLoadsSubmitted = materialLoads.submittedCount;
    m_materialLoadsPending = materialLoads.submittedCount > 0u;

    const fuse::project::T3DShaderVfsResolveResult shaderVfs =
        fuse::project::resolveT3DShaderVfsFromBindings(materialBindings);
    m_embedSession.shaderVfsResolved = shaderVfs.resolvedCount;
    m_embedSession.shaderVfsUnresolved = shaderVfs.unresolvedCount;

    const fuse::project::T3DShaderVfsAsyncLoadResult shaderLoads =
        fuse::project::submitT3DShaderLoadsAsync(materialBindings, &m_shaderCookCache);
    m_embedSession.shaderCookCacheHits = shaderLoads.cookCacheHits;
    m_embedSession.shaderAsyncLoadsSubmitted = shaderLoads.submittedCount;
    m_shaderLoadsPending = shaderLoads.submittedCount > 0u;

    m_embedSession.worldLoaded = true;
    m_embedded = true;

#if defined(FUSE_VULKAN_BACKEND)
    m_loadedManifest = projectLoad.manifest;
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu != nullptr) {
        attachEmbedWorldsToHybrid(gpu, m_embedSession, m_loadedManifest);
    }
#endif

    if (!m_lastProjectLabel.empty()) {
        runtimeScene.setName(m_lastProjectLabel);
    }
}

void RuntimeViewportHook::drainPendingMaterialLoads_() {
    if (!m_materialLoadsPending) {
        return;
    }

    if (fuse::io::VirtualFileSystem::instance().completedLoadCount() == 0u) {
        return;
    }

    const fuse::project::T3DMaterialCookCacheResult drained =
        fuse::project::drainT3DMaterialLoads(m_materialAssetTable, &m_materialCookCache);
    m_embedSession.materialAsyncLoadsDrained += drained.drainedCount;
    m_embedSession.materialCookCacheStores += drained.cookCacheStores;
    m_embedSession.materialTextureCooks += drained.cookCacheStores;
    m_materialAssetTable.commit();
    if (fuse::io::VirtualFileSystem::instance().completedLoadCount() == 0u) {
        m_materialLoadsPending = false;
    }
    bindCookedAssetsToHybrid_();
}

void RuntimeViewportHook::drainPendingShaderLoads_() {
    if (!m_shaderLoadsPending) {
        return;
    }

    if (fuse::io::VirtualFileSystem::instance().completedLoadCount() == 0u) {
        return;
    }

    const fuse::project::T3DShaderCookCacheResult drained =
        fuse::project::drainT3DShaderLoads(m_shaderAssetTable, &m_shaderCookCache);
    m_embedSession.shaderAsyncLoadsDrained += drained.drainedCount;
    m_embedSession.shaderCookCacheStores += drained.cookCacheStores;
    m_shaderAssetTable.commit();
    if (fuse::io::VirtualFileSystem::instance().completedLoadCount() == 0u) {
        m_shaderLoadsPending = false;
    }
    bindCookedAssetsToHybrid_();
}

void RuntimeViewportHook::bindCookedAssetsToHybrid_() {
#if defined(FUSE_VULKAN_BACKEND)
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu == nullptr || gpu->hybrid == nullptr) {
        return;
    }

    fuse::hybrid::CookedAssetBindings& bindings = gpu->hybrid->composer().cookedAssets();
    bindings.clear();

    fuse::io::VirtualFileSystem& vfs = fuse::io::VirtualFileSystem::instance();
    auto resolveCookProbePath = [&](const std::string& cookOutput) -> std::string {
        if (cookOutput.empty()) {
            return {};
        }
        if (!m_embedSession.projectRoot.empty()) {
            const std::string joined = m_embedSession.projectRoot + "/" + cookOutput;
            if (std::ifstream(joined)) {
                return joined;
            }
        }
        std::string physicalPath;
        if (vfs.resolve(cookOutput, physicalPath)) {
            return physicalPath;
        }
        if (vfs.resolve("/game/" + cookOutput, physicalPath)) {
            return physicalPath;
        }
        return cookOutput;
    };

    for (const fuse::io::CompletedLoad& load : vfs.lastDrainedLoads()) {
        if (!load.success || load.asset.virtualPath.empty()) {
            continue;
        }

        const std::string materialOutput =
            fuse::project::materialVirtualPathToCookOutput(load.asset.virtualPath);
        if (!materialOutput.empty()) {
            const std::string refName =
                fuse::project::materialVirtualPathToRefName(load.asset.virtualPath);
            const u32 materialId = fuse::scene::hashWireRefName(refName);
            bindings.bindMaterial(resolveCookProbePath(materialOutput), materialId);
            continue;
        }

        const std::string shaderOutput =
            fuse::project::shaderVirtualPathToCookOutput(load.asset.virtualPath);
        if (!shaderOutput.empty()) {
            const std::string refName =
                fuse::project::shaderVirtualPathToRefName(load.asset.virtualPath);
            const u32 shaderId = fuse::scene::hashWireRefName(refName);
            bindings.bindShader(resolveCookProbePath(shaderOutput), shaderId);
        }
    }

    bindings.refreshTints();
    m_embedSession.cookedMaterialBindings = bindings.materialCount();
    m_embedSession.cookedShaderBindings = bindings.shaderCount();
    m_embedSession.cookedMaterialValidBindings = bindings.validMaterialCount();
    m_embedSession.cookedShaderValidBindings = bindings.validShaderCount();
    if (bindings.validMaterialCount() > 0u || bindings.validShaderCount() > 0u) {
        ++m_embedSession.cookedAssetTintApplied;
    }
#else
    (void)0;
#endif
}

#if defined(FUSE_VULKAN_BACKEND)
namespace {
template <typename Hint>
void applyCatalogColor(const MaterialCatalog& materials, u32 id, Hint& hint) {
    if (!materials.valid(id)) {
        return;
    }
    const MaterialEditState& m = materials.entry(id).state;
    auto to8 = [](f32 v) { return static_cast<u8>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
    hint.hasColor = true;
    hint.r = to8(m.baseColorR);
    hint.g = to8(m.baseColorG);
    hint.b = to8(m.baseColorB);
}
} // namespace
#endif

void RuntimeViewportHook::syncMeshSdfPreviewFromEcs_(EditorHost& host) {
#if defined(FUSE_VULKAN_BACKEND)
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu == nullptr || gpu->hybrid == nullptr || !gpu->worldsAttached) {
        return;
    }

    fuse::hybrid::MeshSdfPreviewCatalog& catalog = gpu->hybrid->composer().previewCatalog();
    catalog.clear();

    host.editorScene().registry().each_query<ecs::Transform>(
        [&](ecs::EntityID id, ecs::Transform& transform) {
            if (host.editorScene().registry().has<ecs::Mesh>(id)) {
                const ecs::Mesh& mesh = *host.editorScene().registry().get<ecs::Mesh>(id);
                if (!mesh.visible) {
                    return;
                }

                fuse::hybrid::MeshPreviewHint hint{};
                hint.x = transform.position.x;
                hint.y = transform.position.y;
                hint.z = transform.position.z;
                hint.materialId = mesh.material_id;
                hint.cookedMeshPath = "cooked/mesh/preview_" + std::to_string(mesh.material_id) + ".fusemesh";
                const std::optional<fuse::hybrid::CookedMaterialBinding> cookedMaterial =
                    gpu->hybrid->composer().cookedAssets().findMaterial(mesh.material_id);
                if (cookedMaterial.has_value()) {
                    hint.cookedMeshPath = cookedMaterial->cookedPath;
                    hint.cookedMeshResolved = true;
                }
                hint.visible = true;
                applyCatalogColor(host.materials(), mesh.material_id, hint);
                catalog.addMesh(hint);
                return;
            }

            if (host.editorScene().registry().has<ecs::SDFObject>(id)) {
                const ecs::SDFObject& sdf = *host.editorScene().registry().get<ecs::SDFObject>(id);
                if (!sdf.visible) {
                    return;
                }

                fuse::hybrid::SdfPreviewHint hint{};
                hint.x = transform.position.x;
                hint.y = transform.position.y;
                hint.z = transform.position.z;
                hint.materialId = sdf.material_id;
                hint.param0 = sdf.params.x;
                hint.param1 = sdf.params.y;
                hint.param2 = sdf.params.z;
                switch (sdf.type) {
                case ecs::SDFPrimitive::Box:
                    hint.primitive = fuse::hybrid::SdfPreviewPrimitive::Box;
                    break;
                case ecs::SDFPrimitive::Capsule:
                    hint.primitive = fuse::hybrid::SdfPreviewPrimitive::Capsule;
                    break;
                case ecs::SDFPrimitive::Torus:
                    hint.primitive = fuse::hybrid::SdfPreviewPrimitive::Torus;
                    break;
                case ecs::SDFPrimitive::Cylinder:
                    hint.primitive = fuse::hybrid::SdfPreviewPrimitive::Cylinder;
                    break;
                case ecs::SDFPrimitive::Custom:
                    hint.primitive = fuse::hybrid::SdfPreviewPrimitive::Custom;
                    break;
                default:
                    hint.primitive = fuse::hybrid::SdfPreviewPrimitive::Sphere;
                    break;
                }
                hint.visible = true;
                applyCatalogColor(host.materials(), sdf.material_id, hint);
                catalog.addSdf(hint);
            }
        });

    m_embedSession.meshPreviewHints = catalog.meshCount();
    m_embedSession.sdfPreviewHints = catalog.sdfCount();
    m_embedSession.meshPreviewCookedResolved = catalog.cookedMeshResolvedCount();
#else
    (void)host;
#endif
}

void RuntimeViewportHook::onSceneFileOpened(ecs::Registry& registry) {
    u32 transformCount = 0;
    registry.each_query<ecs::Transform>([&](ecs::EntityID /*id*/, ecs::Transform& /*transform*/) {
        ++transformCount;
    });
    m_embedSession.mirroredEditorEntityCount = transformCount;
}

void RuntimeViewportHook::mirrorEditorEntities_(EditorHost& host) {
    if (m_embedSession.worldLoaded) {
        return;
    }

    // Reused scratch: idle editor ticks must not allocate (fuse_editor_b6_idle_frame_gates).
    std::vector<ecs::EntityID>& entityOrder = m_entityOrderScratch;
    entityOrder.clear();
    host.editorScene().registry().each_query<ecs::Transform>(
        [&](ecs::EntityID id, ecs::Transform& /*transform*/) { entityOrder.push_back(id); });

    const u32 aliveCount = static_cast<u32>(entityOrder.size());
    if (aliveCount == m_embedSession.mirroredEditorEntityCount) {
        return;
    }

    if (aliveCount == 0) {
        m_embedSession.mirroredEditorEntityCount = 0;
        return;
    }

    auto findRuntimeIndex = [&](ecs::EntityID id) -> s32 {
        for (u32 index = 0; index < entityOrder.size(); ++index) {
            if (entityOrder[index] == id) {
                return static_cast<s32>(index);
            }
        }
        return -1;
    };

    fuse::scene::Scene& runtimeScene = host.runtimeScene();
    runtimeScene.clearEntities();
    for (u32 index = 0; index < entityOrder.size(); ++index) {
        const ecs::EntityID id = entityOrder[index];
        const ecs::Transform& transform = *host.editorScene().registry().get<ecs::Transform>(id);

        fuse::scene::SceneEntityTransform sceneTransform{};
        sceneTransform.positionX = transform.position.x;
        sceneTransform.positionY = transform.position.y;
        sceneTransform.positionZ = transform.position.z;
        sceneTransform.rotationX = transform.rotation.x;
        sceneTransform.rotationY = transform.rotation.y;
        sceneTransform.rotationZ = transform.rotation.z;
        sceneTransform.rotationW = transform.rotation.w;
        sceneTransform.scaleX = transform.scale.x;
        sceneTransform.scaleY = transform.scale.y;
        sceneTransform.scaleZ = transform.scale.z;

        s32 parentIndex = -1;
        if (transform.parent.valid()) {
            parentIndex = findRuntimeIndex(transform.parent);
        }

        const std::string name = "EditorEntity_" + std::to_string(index);
        runtimeScene.addEntity(name, sceneTransform, parentIndex);
    }

    m_embedSession.mirroredEditorEntityCount = aliveCount;
    m_embedded = true;
}

void RuntimeViewportHook::syncEcsToEmbedWorld3D_(EditorHost& host) {
#if defined(FUSE_VULKAN_BACKEND)
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu == nullptr || !gpu->worldsAttached) {
        return;
    }

    // E03: the viewport renders the edited scene itself: World3D::render hands the editor's ECS registry
    // (Transform + Mesh + lights) and the viewport fly camera to the hybrid SceneRenderer (GPU path; the
    // PlaceholderRenderer only without a Vulkan device).
    // World matrices must be current: the GPU extractor reads Transform::local_to_world, and editor
    // edits (inspector, gizmo, scripted placement) only mark Transform::dirty. Free when nothing is dirty.
    ecs::TransformSystem::update(host.editorScene().registry());
    gpu->embedWorld3D.setRenderRegistry(&host.editorScene().registry());
    {
        const ViewportCamera& cam = m_panel.camera();
        const ecs::vec3 forward = m_panel.forward();
        fuse::world3d::RenderCamera3D view{};
        view.eye[0] = cam.positionX;
        view.eye[1] = cam.positionY;
        view.eye[2] = cam.positionZ;
        view.target[0] = cam.positionX + forward.x;
        view.target[1] = cam.positionY + forward.y;
        view.target[2] = cam.positionZ + forward.z;
        view.fovY = cam.fovDeg * 0.017453292519943295f;
        view.nearPlane = cam.nearPlane > 0.f ? cam.nearPlane : 0.1f;
        view.farPlane = (std::min)(cam.farPlane > view.nearPlane ? cam.farPlane : 1000.f, 2000.f);
        view.valid = true;
        gpu->embedWorld3D.setCamera(view);
    }
    // GUI-3: the Material Editor catalog is the World3D material table (row id == Mesh::material_id).
    if (host.materials().revision() != m_materialRevisionSeen) {
        m_materialRevisionSeen = host.materials().revision();
        m_embedSession.catalogMaterialCount = host.materials().applyTo(gpu->embedWorld3D);
        ++m_embedSession.catalogMaterialSyncs;
    }

    // Reused scratch: idle editor ticks must not allocate (fuse_editor_b6_idle_frame_gates).
    std::vector<ecs::EntityID>& entityOrder = m_entityOrderScratch;
    entityOrder.clear();
    host.editorScene().registry().each_query<ecs::Transform>(
        [&](ecs::EntityID id, ecs::Transform& /*transform*/) { entityOrder.push_back(id); });

    const u32 aliveCount = static_cast<u32>(entityOrder.size());
    if (aliveCount == m_embedSession.ecsWorld3DObjectCount &&
        aliveCount == gpu->ecsMirrorObjects.size()) {
        for (u32 index = 0; index < aliveCount; ++index) {
            const ecs::Transform& transform =
                *host.editorScene().registry().get<ecs::Transform>(entityOrder[index]);
            fuse::SceneObject3D* object = gpu->ecsMirrorObjects[index].get();
            if (object == nullptr) {
                continue;
            }
            applyEcsTransformToSceneObject3D(transform, *object);
        }
        ++m_embedSession.ecsWorld3DSyncTicks;
        return;
    }

    gpu->embedWorld3D.clearDynamicObjects();
    gpu->ecsMirrorObjects.clear();
    gpu->ecsMirrorObjects.reserve(entityOrder.size());

    for (u32 index = 0; index < entityOrder.size(); ++index) {
        const ecs::EntityID id = entityOrder[index];
        const ecs::Transform& transform = *host.editorScene().registry().get<ecs::Transform>(id);

        auto object = std::make_unique<fuse::SceneObject3D>("EcsMirror_" + std::to_string(index));
        applyEcsTransformToSceneObject3D(transform, *object);
        gpu->embedWorld3D.addObject(object.get());
        gpu->ecsMirrorObjects.push_back(std::move(object));
    }

    m_embedSession.ecsWorld3DObjectCount = aliveCount;
    ++m_embedSession.ecsWorld3DSyncTicks;
#else
    (void)host;
#endif
}

#if defined(FUSE_VULKAN_BACKEND)
void recreateHybridForExternalSurface(RuntimeViewportHook& hook, RuntimeViewportHeadlessGpuStub* gpu) {
    if (gpu == nullptr || hook.swapchainHandoff().nativeSurface == nullptr ||
        !hook.swapchainHandoff().qtRealSurface) {
        return;
    }

    if (gpu->hybrid != nullptr) {
        gpu->hybrid->shutdown();
        gpu->hybrid.reset();
        hook.embedSession().wsiPresentPathReady = false;
    }

    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    bootstrapDesc.presentable.backend = fuse::hybrid::PresentableBackend::Headless;
    bootstrapDesc.presentable.swapchainWidth = hook.swapchainHandoff().width > 0 ? hook.swapchainHandoff().width
                                                                                  : hook.panel().width();
    bootstrapDesc.presentable.swapchainHeight =
        hook.swapchainHandoff().height > 0 ? hook.swapchainHandoff().height : hook.panel().height();
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
    bootstrapDesc.renderer.rhi.bootstrap.createSwapchain = true;
    bootstrapDesc.scene.sdfProxyMeshes = true; // editor: sculpted SDFs visible in the raster viewport
    bootstrapDesc.renderer.rhi.bootstrap.swapchain = hook.buildSwapchainDescHandoff();
    // The hybrid bootstrap creates its own VkInstance, which cannot own the handed-off surface
    // (VkSurfaceKHR is per-instance). Start it headless; syncHybridBootstrapFromConsumedHandoff
    // only attaches the surface when the instances match.
    bootstrapDesc.renderer.rhi.bootstrap.swapchain.surface = fuse::renderer::SurfaceDesc{};

    gpu->hybrid = fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
    if (gpu->hybrid != nullptr && gpu->hybrid->isReady()) {
        hook.embedSession().wsiPresentPathReady = true;
        hook.embedSession().qtLivePresentReady = true;
        hook.embedSession().headlessGpuReady = true;
    }
}
#endif

void RuntimeViewportHook::tickHeadlessPresentStub_() {
    if (!m_embedded) {
        return;
    }

    m_embedSession.wsiBackendName = fuse::platform::windowWsiBackendName();
    // The native X11 and Win32 backends only serve the standalone game window; the editor viewport
    // stays Qt-owned, so it renders through the headless GPU path like the Null WSI (until a Qt
    // surface handoff wires an external swapchain, which clears this below).
    const fuse::platform::WindowWsiKind wsi = fuse::platform::activeWindowWsiKind();
    m_embedSession.usesHeadlessGpuPath = wsi == fuse::platform::WindowWsiKind::Null ||
                                         wsi == fuse::platform::WindowWsiKind::X11 ||
                                         wsi == fuse::platform::WindowWsiKind::Win32;
    ++m_embedSession.headlessPresentTicks;

#if defined(FUSE_VULKAN_BACKEND)
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu == nullptr) {
        m_headlessGpuStub = new RuntimeViewportHeadlessGpuStub();
        gpu = asHeadlessGpuStub(m_headlessGpuStub);
    }

    if (!m_embedSession.wsiPresentPathReady && gpu->hybrid == nullptr) {
        fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
        bootstrapDesc.presentable.backend = fuse::hybrid::PresentableBackend::Headless;
        bootstrapDesc.presentable.swapchainWidth = m_panel.width() > 0 ? m_panel.width() : 640u;
        bootstrapDesc.presentable.swapchainHeight = m_panel.height() > 0 ? m_panel.height() : 480u;
        bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = false;
        bootstrapDesc.renderer.rhi.bootstrap.createSwapchain = true;
        bootstrapDesc.scene.sdfProxyMeshes = true; // editor: sculpted SDFs visible in the raster viewport
        if (m_surfaceHandoff.nativeSurface != nullptr) {
            bootstrapDesc.presentable.swapchainWidth = m_surfaceHandoff.width;
            bootstrapDesc.presentable.swapchainHeight = m_surfaceHandoff.height;
            bootstrapDesc.renderer.rhi.bootstrap.swapchain = buildSwapchainDescHandoff();
        }
        gpu->hybrid = fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
        if (gpu->hybrid != nullptr && gpu->hybrid->isReady()) {
            m_embedSession.wsiPresentPathReady = true;
            m_embedSession.headlessGpuReady = true;
            // No project yet: still attach (3D) so the edited scene renders on the GPU path.
            attachEmbedWorldsToHybrid(gpu, m_embedSession, m_loadedManifest);
#if defined(FUSE_HAS_VULKAN_RHI)
            syncHybridBootstrapFromConsumedHandoff(*gpu->hybrid, m_surfaceHandoff);
            maybeRetireSoftwarePlaceholder(gpu, m_embedSession, m_surfaceHandoff);
#endif
        }
    }

    if (!m_embedSession.headlessGpuReady && gpu->context == nullptr) {
        fuse::renderer::RhiContext::Desc desc{};
        desc.bootstrap.instance.enableValidation = false;
        desc.enableRasterPath = false;
        desc.enableCompositePass = false;
        gpu->context = fuse::renderer::RhiContext::create(desc);
        if (gpu->context != nullptr && gpu->context->bootstrap().status().deviceReady) {
            m_embedSession.headlessGpuReady = true;
        }
    }

    if (m_embedSession.wsiPresentPathReady && gpu->hybrid != nullptr) {
        if (m_surfaceHandoff.qtRealSurface && m_embedSession.usesExternalSwapchain) {
            ++m_embedSession.qtLivePresentAttempts;
        }

        fuse::frame::FrameCtx frameCtx{};
        frameCtx.frameIndex = m_runtimeTickCount;
        ensureMaterialTextureLoader(gpu, m_embedSession);
        const u32 composerFramesBefore = gpu->hybrid->composer().frameCount();
        gpu->hybrid->runFrame(frameCtx);
        if (gpu->hybrid->composer().frameCount() > composerFramesBefore) {
            ++m_embedSession.hybridComposerFrames;
            m_embedSession.ecsWorld3DSnapshotVisible = gpu->embedWorld3D.readSnapshot().visibleCount();
            m_embedSession.meshPreviewDraws = gpu->hybrid->composer().meshPreviewDraws();
            m_embedSession.sdfPreviewDraws = gpu->hybrid->composer().sdfPreviewDraws();
        }

        fuse::renderer::PresentPath* presentPath = gpu->hybrid->presentPath();
        if (presentPath != nullptr) {
            ++m_embedSession.wsiPresentPathTicks;
            ++gpu->submittedFrames;
            m_embedSession.submittedFrames = gpu->submittedFrames;
#if defined(FUSE_HAS_VULKAN_RHI)
            maybeRetireSoftwarePlaceholder(gpu, m_embedSession, m_surfaceHandoff);
            if (viewportQtPresentPathReady(m_surfaceHandoff, gpu->externalSwapchainWired)) {
                ++m_embedSession.qtPresentPathReadyTicks;
            }
            if (viewportQtPresentPathEligible(m_surfaceHandoff, gpu->externalSwapchainWired)) {
                ++m_embedSession.qtPresentPathEligibleTicks;
            }
            if (viewportQtPresentEligible(m_surfaceHandoff)) {
                ++m_embedSession.qtPresentEligibleTicks;
            }
            if (presentPath->status().realPresentCallCount > m_embedSession.realPresentCallCount) {
                m_embedSession.realPresentCallCount = presentPath->status().realPresentCallCount;
            }
            if (presentPath->status().qtRealPresentCallCount > m_embedSession.qtRealPresentCallCount) {
                m_embedSession.qtRealPresentCallCount = presentPath->status().qtRealPresentCallCount;
            }
            if (m_surfaceHandoff.qtRealSurface && m_embedSession.usesExternalSwapchain &&
                presentPath->status().qtRealPresentCallCount > 0u) {
                ++m_embedSession.qtLivePresentTicks;
            }
            if (presentPath->status().presentSkippedNoWsiCount > m_embedSession.presentSkippedNoWsiCount) {
                m_embedSession.presentSkippedNoWsiCount = presentPath->status().presentSkippedNoWsiCount;
            }
#endif
        }
        return;
    }

    if (m_embedSession.headlessGpuReady && gpu->context != nullptr) {
        fuse::renderer::RenderCommandList commands;
        if (gpu->context->beginFrame(0) && gpu->context->submitFrame(commands, 0)) {
            ++gpu->submittedFrames;
            m_embedSession.submittedFrames = gpu->submittedFrames;
            ++m_embedSession.wsiPresentPathTicks;
        }
    }

#endif
}

void RuntimeViewportHook::requestWindowSystemPresent(const WindowPresentRequest& request) {
    if (m_windowPresentState.load(std::memory_order_acquire) != WindowPresentState::Off) {
        return; // one request per hook; the game thread may already be reading it
    }
    WindowPresentState expected = WindowPresentState::Off;
    m_windowPresentRequest = request;
#if defined(FUSE_VULKAN_BACKEND)
    m_windowPresentState.compare_exchange_strong(expected, WindowPresentState::InstancePending,
                                                 std::memory_order_acq_rel);
#else
    m_windowPresentState.compare_exchange_strong(expected, WindowPresentState::Failed,
                                                 std::memory_order_acq_rel);
#endif
}

WindowPresentStats RuntimeViewportHook::windowPresentStats() const {
    WindowPresentStats stats{};
    stats.frames = m_wpFrames.load(std::memory_order_acquire);
    stats.acquiredImages = m_wpAcquired.load(std::memory_order_acquire);
    stats.presentedImages = m_wpPresented.load(std::memory_order_acquire);
    stats.swapchainRecreates = m_wpRecreates.load(std::memory_order_acquire);
    stats.width = m_wpWidth.load(std::memory_order_acquire);
    stats.height = m_wpHeight.load(std::memory_order_acquire);
    return stats;
}

bool RuntimeViewportHook::windowPresentActive_() const {
    const WindowPresentState state = m_windowPresentState.load(std::memory_order_acquire);
    return state == WindowPresentState::InstancePending || state == WindowPresentState::InstanceReady ||
           state == WindowPresentState::SurfaceWired;
}

void RuntimeViewportHook::releaseWindowSurface() {
#if defined(FUSE_VULKAN_BACKEND)
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu != nullptr && gpu->hybrid != nullptr && gpu->externalSwapchainWired) {
        fuse::renderer::RhiContext* rhi = gpu->hybrid->rendererBootstrap().rhiContext();
        if (rhi != nullptr) {
            drainViewportGpuContext(*rhi);
            // Swapchain before the UI-owned surface: replace it with a headless one.
            fuse::renderer::SwapchainDesc headless{};
            headless.width = m_panel.width() > 0 ? m_panel.width() : 64u;
            headless.height = m_panel.height() > 0 ? m_panel.height() : 64u;
            (void)rhi->bootstrap().ensureSwapchain(headless);
        }
        gpu->externalSwapchainWired = false;
    }
#endif
    m_surfaceHandoff = {};
    m_embedSession.usesExternalSwapchain = false;
    WindowPresentState wired = WindowPresentState::SurfaceWired;
    if (m_windowPresentState.compare_exchange_strong(wired, WindowPresentState::InstanceReady,
                                                     std::memory_order_acq_rel)) {
        fuse::core::setTrackBHostFeature(fuse::core::TrackBHostFeature::EditorViewportPresent, false);
    }
}

void RuntimeViewportHook::tickWindowPresent_() {
#if defined(FUSE_VULKAN_BACKEND)
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu == nullptr) {
        m_headlessGpuStub = new RuntimeViewportHeadlessGpuStub();
        gpu = asHeadlessGpuStub(m_headlessGpuStub);
    }

    // 1. One renderer for the viewport, on an instance the UI can build its window surface on
    //    (built off the scene lock by the game thread's prewarm when available).
    if (!gpu->windowBringUpDone) {
        if (gpu->hybrid == nullptr) {
            auto token = std::make_shared<WindowPrewarmToken>();
            token->request = m_windowPresentRequest;
            token->width = m_panel.width() > 0 ? m_panel.width() : 640u;
            token->height = m_panel.height() > 0 ? m_panel.height() : 480u;
            buildWindowPresentPrewarm(token.get());
            gpu->hybrid = std::move(token->hybrid);
            gpu->instanceRequest = std::move(token);
        }

        fuse::renderer::RhiContext* rhi =
            gpu->hybrid != nullptr && gpu->hybrid->isReady() ? gpu->hybrid->rendererBootstrap().rhiContext() : nullptr;
        const fuse::renderer::VulkanInstance* instance = rhi != nullptr ? rhi->bootstrap().instance() : nullptr;
        bool wsiReady = instance != nullptr && instance->isValid() && rhi->bootstrap().status().deviceReady &&
                        instance->info().instanceHasExtension("VK_KHR_surface");
        for (const std::string& name : m_windowPresentRequest.instanceExtensions) {
            wsiReady = wsiReady && instance->info().instanceHasExtension(name.c_str());
        }
        if (gpu->hybrid != nullptr && gpu->hybrid->isReady()) {
            m_embedSession.wsiPresentPathReady = true;
            m_embedSession.headlessGpuReady = true;
            // No project yet: still attach (3D) so the edited scene renders on the GPU path.
            attachEmbedWorldsToHybrid(gpu, m_embedSession, m_loadedManifest);
        } else {
            gpu->hybrid.reset();
        }
        if (!wsiReady) {
            fuse::log::warn("RuntimeViewportHook: no Vulkan instance with the window-system extensions — "
                            "viewport stays headless");
            m_windowPresentState.store(WindowPresentState::Failed, std::memory_order_release);
            return;
        }
        m_windowPresentInstance.store(instance->nativeHandle(), std::memory_order_release);
        m_windowPresentState.store(WindowPresentState::InstanceReady, std::memory_order_release);
        gpu->windowBringUpDone = true;
    }

    fuse::renderer::RhiContext* rhi = gpu->hybrid->rendererBootstrap().rhiContext();

    // 2. The UI handed back a surface created on that instance: build the real swapchain on it.
    //    (A winId stub hand-off is not a VkSurfaceKHR: consume it and keep waiting.)
    if (m_surfaceHandoff.pending && !m_surfaceHandoff.qtRealSurface) {
        m_surfaceHandoff.pending = false;
        m_surfaceHandoff.consumed = true;
        m_embedSession.surfaceHandoffPending = false;
    }
    if (m_surfaceHandoff.pending) {
        ++m_embedSession.swapchainWiringAttempts;
        drainViewportGpuContext(*rhi);
        const ViewportSwapchainWiringResult wiring = wireExternalSwapchainFromHandoff(*rhi, m_surfaceHandoff);
        m_embedSession.surfaceHandoffPending = false;
        m_embedSession.surfaceHandoffConsumed = wiring.attempted;
        if (wiring.swapchainReady) {
            ++m_embedSession.swapchainWiringReady;
            m_embedSession.usesExternalSwapchain = true;
            m_embedSession.usesHeadlessGpuPath = false;
            m_embedSession.qtLivePresentReady = true;
            gpu->externalSwapchainWired = true;
            const fuse::renderer::VulkanSwapchain* swapchain = rhi->bootstrap().swapchain();
            m_wpWidth.store(swapchain->info().width, std::memory_order_release);
            m_wpHeight.store(swapchain->info().height, std::memory_order_release);
            // Editor-scoped Track B unlock: this process presents its own viewport surface.
            fuse::core::setTrackBHostFeature(fuse::core::TrackBHostFeature::EditorViewportPresent, true);
            m_windowPresentState.store(WindowPresentState::SurfaceWired, std::memory_order_release);
            maybeRetireSoftwarePlaceholder(gpu, m_embedSession, m_surfaceHandoff);
        } else {
            fuse::log::warn("RuntimeViewportHook: viewport surface not presentable (%s) — headless path",
                            wiring.note != nullptr ? wiring.note : "?");
            gpu->externalSwapchainWired = false;
            m_windowPresentState.store(WindowPresentState::Failed, std::memory_order_release);
            return;
        }
    }

    // 3. Render + present one viewport frame (fence wait -> acquire -> record/submit -> present).
    fuse::renderer::PresentPath* presentPath = gpu->hybrid->presentPath();
    if (presentPath == nullptr) {
        return;
    }
    const fuse::renderer::PresentPathStatus& before = presentPath->status();
    const u64 acquiredBefore = before.acquiredImageCount;
    const u32 realPresentsBefore = before.realPresentCallCount;

    fuse::frame::FrameCtx frameCtx{};
    frameCtx.frameIndex = m_runtimeTickCount;
    ensureMaterialTextureLoader(gpu, m_embedSession);
    const u32 composerFramesBefore = gpu->hybrid->composer().frameCount();
    gpu->hybrid->runFrame(frameCtx);
    if (gpu->hybrid->composer().frameCount() > composerFramesBefore) {
        ++m_embedSession.hybridComposerFrames;
        m_embedSession.ecsWorld3DSnapshotVisible = gpu->embedWorld3D.readSnapshot().visibleCount();
        m_embedSession.meshPreviewDraws = gpu->hybrid->composer().meshPreviewDraws();
        m_embedSession.sdfPreviewDraws = gpu->hybrid->composer().sdfPreviewDraws();
    }
    ++gpu->submittedFrames;
    m_embedSession.submittedFrames = gpu->submittedFrames;
    ++m_embedSession.wsiPresentPathTicks;

    const fuse::renderer::PresentPathStatus& after = presentPath->status();
    if (gpu->externalSwapchainWired) {
        m_wpFrames.fetch_add(1, std::memory_order_acq_rel);
        m_wpAcquired.fetch_add(after.acquiredImageCount - acquiredBefore, std::memory_order_acq_rel);
        m_wpPresented.fetch_add(after.realPresentCallCount - realPresentsBefore, std::memory_order_acq_rel);
        ++m_embedSession.qtLivePresentAttempts;
        if (after.realPresentCallCount > realPresentsBefore) {
            ++m_embedSession.qtLivePresentTicks;
        }
    }
    m_embedSession.realPresentCallCount = after.realPresentCallCount;
    m_embedSession.qtRealPresentCallCount = after.qtRealPresentCallCount;
    m_embedSession.presentSkippedNoWsiCount = after.presentSkippedNoWsiCount;
#endif
}

void RuntimeViewportHook::tick(EditorHost& host, f32 dt) {
    applyPendingResize_();

    if (!m_embedSession.qVulkanWindowWsiProbed) {
        const QVulkanWindowWsiProbeResult probe = probeQVulkanWindowWsi();
        m_embedSession.qVulkanWindowWsiProbed = probe.attempted || probe.headlessSkipped;
        m_embedSession.qVulkanWindowWsiReady = probe.surfaceReady;
        m_embedSession.qVulkanInstanceReady = probe.instanceReady;
        m_embedSession.qVulkanExtensionsProbed = probe.extensionsProbed;
        m_embedSession.qVulkanSupportedExtensionCount = probe.supportedExtensionCount;
        m_embedSession.qVulkanWindowCreated = probe.windowCreated;
        m_embedSession.qVulkanWindowDestroyed = probe.windowDestroyed;
        m_embedSession.qVulkanInstanceVersionMajor = probe.instanceVersionMajor;
        m_embedSession.qVulkanInstanceVersionMinor = probe.instanceVersionMinor;
    }

    ensureWorldLoaded_(host);
    drainPendingMaterialLoads_();
    drainPendingShaderLoads_();
    mirrorEditorEntities_(host);
    syncEcsToEmbedWorld3D_(host);
    syncMeshSdfPreviewFromEcs_(host);
    syncTerrainMeshes(host.editorScene().registry());
    fillMissingMeshBounds(host.editorScene().registry());

    if (!m_lastProjectLabel.empty()) {
        if (host.runtimeScene().name() != m_lastProjectLabel) {
            host.runtimeScene().setName(m_lastProjectLabel);
        }
        m_embedded = true;
    }

    m_panel.tick(dt);

#if defined(FUSE_VULKAN_BACKEND)
    if (windowPresentActive_()) {
        tickWindowPresent_();
        if (windowPresentActive_()) {
            ++m_runtimeTickCount;
            return;
        }
        // Instance / surface bring-up failed: fall through to the headless path this tick.
    }
    if (m_surfaceHandoff.pending) {
        RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
        if (gpu == nullptr) {
            m_headlessGpuStub = new RuntimeViewportHeadlessGpuStub();
            gpu = asHeadlessGpuStub(m_headlessGpuStub);
        }

        if (gpu->context == nullptr) {
            fuse::renderer::RhiContext::Desc desc{};
            desc.bootstrap.instance.enableValidation = false;
            desc.enableRasterPath = false;
            desc.enableCompositePass = false;
            gpu->context = fuse::renderer::RhiContext::create(desc);
            if (gpu->context != nullptr && gpu->context->bootstrap().status().deviceReady) {
                m_embedSession.headlessGpuReady = true;
            }
        }

        ++m_embedSession.swapchainWiringAttempts;
        if (gpu->context != nullptr) {
            const ViewportSwapchainWiringResult wiring =
                wireExternalSwapchainFromHandoff(*gpu->context, m_surfaceHandoff);
            m_embedSession.surfaceHandoffPending = false;
            m_embedSession.surfaceHandoffConsumed = wiring.attempted;
            if (wiring.swapchainReady) {
                ++m_embedSession.swapchainWiringReady;
                m_embedSession.usesExternalSwapchain = true;
                gpu->externalSwapchainWired = true;
                m_embedSession.usesHeadlessGpuPath = false;
                if (m_surfaceHandoff.qtRealSurface) {
                    recreateHybridForExternalSurface(*this, gpu);
                }
            }
#if defined(FUSE_HAS_VULKAN_RHI)
            if (gpu->hybrid != nullptr && m_surfaceHandoff.consumed) {
                syncHybridBootstrapFromConsumedHandoff(*gpu->hybrid, m_surfaceHandoff);
                maybeRetireSoftwarePlaceholder(gpu, m_embedSession, m_surfaceHandoff);
            }
#endif
        } else {
            m_surfaceHandoff.pending = false;
            m_surfaceHandoff.consumed = true;
            m_embedSession.surfaceHandoffPending = false;
            m_embedSession.surfaceHandoffConsumed = true;
        }
    }
#else
    if (m_surfaceHandoff.pending) {
        ++m_embedSession.swapchainWiringAttempts;
        m_surfaceHandoff.pending = false;
        m_surfaceHandoff.consumed = true;
        m_embedSession.surfaceHandoffPending = false;
        m_embedSession.surfaceHandoffConsumed = true;
    }
#endif

    tickHeadlessPresentStub_();
    ++m_runtimeTickCount;
}

void RuntimeViewportHook::enterPlayViewportCamera(fuse::ecs::Registry& registry) {
    f32 spawn[3] = {};
    if (!m_playFromSpawn || !fuse::ecs::findFirstActiveSpawnWorldPosition(registry, spawn)) {
        return;
    }
    spawn[1] += 1.7f; // eye height: a ground-level spawn otherwise puts the camera inside the floor
    if (!m_playViewportCameraSaved) {
        m_playViewportCameraSnapshot = m_panel.camera();
        m_playViewportCameraSaved = true;
    }
    m_panel.setFromSpawnView(spawn);
}

void RuntimeViewportHook::exitPlayViewportCamera() {
    if (!m_playViewportCameraSaved) {
        return;
    }
    m_panel.camera() = m_playViewportCameraSnapshot;
    m_playViewportCameraSaved = false;
}

} // namespace fuse::editor

namespace fuse::editor {

std::shared_ptr<void> RuntimeViewportHook::beginWindowPresentPrewarm() {
#if defined(FUSE_VULKAN_BACKEND)
    if (!windowPresentActive_() || m_windowPresentRequest.instanceExtensions.empty()) {
        return nullptr;
    }
    const RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu != nullptr && gpu->hybrid != nullptr) {
        return nullptr;
    }
    auto token = std::make_shared<WindowPrewarmToken>();
    token->request = m_windowPresentRequest;
    token->width = m_panel.width() > 0 ? m_panel.width() : 640u;
    token->height = m_panel.height() > 0 ? m_panel.height() : 480u;
    return token;
#else
    return nullptr;
#endif
}

void RuntimeViewportHook::buildWindowPresentPrewarm(void* tokenPtr) {
#if defined(FUSE_VULKAN_BACKEND)
    auto* token = static_cast<WindowPrewarmToken*>(tokenPtr);
    if (token == nullptr || token->hybrid != nullptr) {
        return;
    }
    std::vector<const char*> extensions;
    extensions.reserve(token->request.instanceExtensions.size());
    for (const std::string& name : token->request.instanceExtensions) {
        extensions.push_back(name.c_str());
    }
    fuse::hybrid::HybridRendererBootstrapDesc bootstrapDesc{};
    bootstrapDesc.presentable.backend = fuse::hybrid::PresentableBackend::Headless;
    bootstrapDesc.presentable.swapchainWidth = token->width;
    bootstrapDesc.presentable.swapchainHeight = token->height;
    bootstrapDesc.renderer.rhi.bootstrap.instance.enableValidation = token->request.enableValidation;
    bootstrapDesc.renderer.rhi.bootstrap.instance.extraExtensions = extensions.data();
    bootstrapDesc.renderer.rhi.bootstrap.instance.extraExtensionCount = static_cast<u32>(extensions.size());
    bootstrapDesc.renderer.rhi.bootstrap.createSwapchain = true;
    bootstrapDesc.scene.sdfProxyMeshes = true; // editor: sculpted SDFs visible in the raster viewport
    token->hybrid = fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
#else
    (void)tokenPtr;
#endif
}

void RuntimeViewportHook::adoptWindowPresentPrewarm(std::shared_ptr<void> tokenPtr) {
#if defined(FUSE_VULKAN_BACKEND)
    auto* token = static_cast<WindowPrewarmToken*>(tokenPtr.get());
    if (token == nullptr || token->hybrid == nullptr || !windowPresentActive_()) {
        return;
    }
    // The UI may have (re)issued the request while we built: only adopt a matching instance.
    if (token->request.instanceExtensions != m_windowPresentRequest.instanceExtensions ||
        token->request.enableValidation != m_windowPresentRequest.enableValidation) {
        return;
    }
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu == nullptr) {
        m_headlessGpuStub = new RuntimeViewportHeadlessGpuStub();
        gpu = asHeadlessGpuStub(m_headlessGpuStub);
    }
    if (gpu->hybrid == nullptr) {
        gpu->hybrid = std::move(token->hybrid); // tickWindowPresent_ step 1 finishes the bring-up
        gpu->instanceRequest = std::static_pointer_cast<WindowPrewarmToken>(std::move(tokenPtr));
    }
#else
    (void)tokenPtr;
#endif
}

} // namespace fuse::editor

namespace fuse::editor {
bool RuntimeViewportHook::copyMeshWireframe(u32 engineId, std::vector<f32>& out) const {
    out.clear();
#if defined(FUSE_VULKAN_BACKEND)
    const RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu == nullptr || gpu->hybrid == nullptr) return false;
    fuse::hybrid::HybridSceneRenderer* scene = gpu->hybrid->composer().gpuScene();
    if (scene == nullptr) return false;
    const std::vector<f32>* edges = scene->sceneRenderer().meshes().wireframeEdges(engineId);
    if (edges == nullptr) return false;
    out = *edges;
    return true;
#else
    (void)engineId;
    return false;
#endif
}
} // namespace fuse::editor

namespace fuse::editor {
u64 RuntimeViewportHook::meshRegistryVersion() const {
#if defined(FUSE_VULKAN_BACKEND)
    const RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu == nullptr || gpu->hybrid == nullptr) return 0;
    fuse::hybrid::HybridSceneRenderer* scene = gpu->hybrid->composer().gpuScene();
    return scene != nullptr ? scene->sceneRenderer().meshes().remapVersion() + 1u : 0u;
#else
    return 0;
#endif
}

void RuntimeViewportHook::syncTerrainMeshes(ecs::Registry& registry) {
#if defined(FUSE_VULKAN_BACKEND)
    fuse::hybrid::HybridSceneRenderer* scene = nullptr;
    if (RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub); gpu != nullptr && gpu->hybrid != nullptr) {
        scene = gpu->hybrid->composer().gpuScene();
    }
#else
    void* scene = nullptr;
#endif
    std::vector<ecs::EntityID> missingMesh;
    registry.each<ecs::Terrain>([&](ecs::EntityID id, ecs::Terrain& terrain) {
        const u64 key = static_cast<u64>(id.index) | (static_cast<u64>(id.generation) << 32);
        const u64 hash = terrainHash(terrain);
        TerrainBuilt& built = m_terrainBuilt[key];
        const u32 engineId = terrainEngineMeshId(id);
        ecs::Mesh* mesh = registry.get<ecs::Mesh>(id);
        const bool meshOk = mesh != nullptr && mesh->vertex_buffer.isValid() && mesh->vertex_buffer.index() == engineId;
        if (built.hash == hash && meshOk && (built.onGpu || scene == nullptr)) return;
        TerrainGeometry geo;
        if (!buildTerrainGeometry(terrain, geo)) return;
        if (mesh == nullptr) {
            missingMesh.push_back(id);
        } else {
            mesh->vertex_buffer = ecs::MeshVertexBufferHandle(engineId, 1u);
            mesh->material_id = terrain.material_id;
            mesh->aabb_min = ecs::vec3{geo.lo[0], geo.lo[1], geo.lo[2], 0.f};
            mesh->aabb_max = ecs::vec3{geo.hi[0], geo.hi[1], geo.hi[2], 0.f};
        }
        built.hash = hash;
        built.onGpu = false;
#if defined(FUSE_VULKAN_BACKEND)
        if (scene != nullptr) {
            fuse::renderer::geometry::MeshletSource src{};
            src.positions = geo.positions.data();
            src.normals = geo.normals.data();
            src.uvs = geo.uvs.data();
            src.vertex_count = static_cast<u32>(geo.positions.size() / 3u);
            src.indices = geo.indices.data();
            src.index_count = static_cast<u32>(geo.indices.size());
            fuse::renderer::geometry::MeshletBuildOptions options{};
            options.backend = fuse::kernel::Backend::CpuReference;
            fuse::renderer::geometry::MeshletMesh meshlets;
            if (fuse::renderer::geometry::build_meshlets(src, options, meshlets, nullptr) &&
                scene->sceneRenderer().meshes().registerMesh(engineId, std::move(meshlets))) {
                built.onGpu = true;
            }
        }
#endif
        ++m_terrainBuilds;
    });
    // Adding components inside each<> would invalidate the iteration; attach missing Mesh rows afterwards
    // (the next sync fills them in).
    for (ecs::EntityID id : missingMesh) {
        ecs::Mesh mesh{};
        registry.add(id, mesh);
        m_terrainBuilt.erase(static_cast<u64>(id.index) | (static_cast<u64>(id.generation) << 32));
    }
    if (!missingMesh.empty()) syncTerrainMeshes(registry);
}
} // namespace fuse::editor

namespace fuse::editor {
u32 RuntimeViewportHook::fillMissingMeshBounds(ecs::Registry& registry) {
    u32 updated = 0;
#if defined(FUSE_VULKAN_BACKEND)
    fuse::hybrid::HybridSceneRenderer* scene = nullptr;
    if (const RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub); gpu != nullptr && gpu->hybrid != nullptr) {
        scene = gpu->hybrid->composer().gpuScene();
    }
#endif
    registry.each<ecs::Mesh>([&](ecs::EntityID id, ecs::Mesh& mesh) {
        const bool degenerate = mesh.aabb_min.x >= mesh.aabb_max.x && mesh.aabb_min.y >= mesh.aabb_max.y &&
                                mesh.aabb_min.z >= mesh.aabb_max.z;
        if (!degenerate) return;
        f32 lo[3] = {}, hi[3] = {};
        bool found = false;
#if defined(FUSE_VULKAN_BACKEND)
        // 1) Real geometry: the registered GPU mesh's vertices.
        if (scene != nullptr && mesh.vertex_buffer.isValid()) {
            const std::vector<f32>* edges = scene->sceneRenderer().meshes().wireframeEdges(mesh.vertex_buffer.index());
            if (edges != nullptr && edges->size() >= 3u) {
                for (int a = 0; a < 3; ++a) lo[a] = hi[a] = (*edges)[a];
                for (usize k = 0; k + 3u <= edges->size(); k += 3u) {
                    for (int a = 0; a < 3; ++a) {
                        lo[a] = (std::min)(lo[a], (*edges)[k + a]);
                        hi[a] = (std::max)(hi[a], (*edges)[k + a]);
                    }
                }
                found = true;
            }
        }
#endif
        // 2) No geometry (e.g. a legacy GroundPlane): use the collider's shape.
        if (!found) {
            if (const ecs::Collider* c = registry.get<ecs::Collider>(id)) {
                const ecs::vec3& q = c->params;
                switch (c->shape) {
                case ecs::Collider::Box:
                    lo[0] = -q.x; lo[1] = -q.y; lo[2] = -q.z; hi[0] = q.x; hi[1] = q.y; hi[2] = q.z;
                    found = q.x > 0.f || q.y > 0.f || q.z > 0.f;
                    break;
                case ecs::Collider::Sphere:
                    for (int a = 0; a < 3; ++a) { lo[a] = -q.x; hi[a] = q.x; }
                    found = q.x > 0.f;
                    break;
                case ecs::Collider::Capsule:
                    lo[0] = lo[2] = -q.x; hi[0] = hi[2] = q.x; lo[1] = -(q.y + q.x); hi[1] = q.y + q.x;
                    found = q.x > 0.f;
                    break;
                case ecs::Collider::Plane: {
                    // Infinite plane: show a 100 x 100 patch through the origin, flat along the normal's
                    // dominant axis.
                    constexpr f32 kHalf = 50.f;
                    const f32 n[3] = {std::fabs(q.x), std::fabs(q.y), std::fabs(q.z)};
                    const int axis = n[1] >= n[0] && n[1] >= n[2] ? 1 : (n[0] >= n[2] ? 0 : 2);
                    for (int a = 0; a < 3; ++a) { lo[a] = -kHalf; hi[a] = kHalf; }
                    lo[axis] = hi[axis] = c->scalar;
                    found = true;
                    break;
                }
                default: break;
                }
            }
        }
        if (!found) return;
        mesh.aabb_min = ecs::vec3{lo[0], lo[1], lo[2], 0.f};
        mesh.aabb_max = ecs::vec3{hi[0], hi[1], hi[2], 0.f};
        ++updated;
    });
    return updated;
}
} // namespace fuse::editor

namespace fuse::editor {
// The editor scene is rebuilt from the level's scene table (names + transforms), which drops the
// level's FECS component block, so wire-bound Mesh rows (e.g. a GroundPlane) arrive without bounds.
// Read the FECS block once and copy each Mesh's local AABB onto the matching editor entity (scene
// entities map in order, skipping wire stubs - the same walk as applyWireBindingsFromScene).
namespace {
template <typename T>
void copyFileComponent(ecs::Registry& from, ecs::Registry& to, u32 fileIndex, ecs::EntityID toId) {
    if (to.has<T>(toId)) return;
    const T* found = nullptr;
    from.each<T>([&](ecs::EntityID id, T& c) {
        if (id.index == fileIndex) found = &c;
    });
    if (found != nullptr) to.add<T>(toId, *found);
}
} // namespace

u32 RuntimeViewportHook::importLevelMeshBounds_(ecs::Registry& editorRegistry, const fuse::scene::Scene& scene,
                                                const std::vector<ecs::EntityID>& entitiesBefore,
                                                const std::string& worldPath) {
    if (worldPath.empty()) return 0;
    fuse::scene::Scene fileScene;
    ecs::Registry fileRegistry;
    if (fuse::scene::SceneSerialiser::loadWithRegistry(worldPath, fileScene, fileRegistry).status !=
        fuse::scene::SerialiseStatus::Ok) {
        return 0;
    }
    std::unordered_map<u32, const ecs::Mesh*> fileMeshByIndex;
    fileRegistry.each<ecs::Mesh>([&](ecs::EntityID id, ecs::Mesh& mesh) { fileMeshByIndex[id.index] = &mesh; });
    std::vector<ecs::EntityID> created;
    editorRegistry.each<ecs::Transform>([&](ecs::EntityID id, ecs::Transform&) {
        if (std::find(entitiesBefore.begin(), entitiesBefore.end(), id) == entitiesBefore.end()) created.push_back(id);
    });
    std::sort(created.begin(), created.end(), [](ecs::EntityID a, ecs::EntityID b) { return a.index < b.index; });
    u32 updated = 0;
    usize k = 0;
    const auto& entities = scene.entities();
    for (usize i = 0; i < entities.size() && k < created.size(); ++i) {
        if (fuse::scene::isWireStubEntityName(entities[i].name)) continue;
        const ecs::EntityID editorId = created[k++];
        // applyWireBindingsFromScene creates identity Transforms; restore the level's placement
        // (e.g. a GroundPlane's kGroundPlaneHalfExtent X/Z scale) so the views match the runtime.
        if (ecs::Transform* t = editorRegistry.get<ecs::Transform>(editorId)) {
            const auto& st = entities[i].transform;
            t->position = ecs::vec3{st.positionX, st.positionY, st.positionZ, 1.f};
            t->rotation = ecs::quat{st.rotationX, st.rotationY, st.rotationZ, st.rotationW};
            t->scale = ecs::vec3{st.scaleX, st.scaleY, st.scaleZ, 0.f};
            t->dirty = true;
        }
        if (i >= fileScene.entities().size() || fileScene.entities()[i].name != entities[i].name) continue;
        // Lighting / environment rows the scene table cannot carry (sun, ambient, fog).
        copyFileComponent<ecs::DirectionalLight>(fileRegistry, editorRegistry, static_cast<u32>(i), editorId);
        copyFileComponent<ecs::AmbientLight>(fileRegistry, editorRegistry, static_cast<u32>(i), editorId);
        copyFileComponent<ecs::EnvironmentFog>(fileRegistry, editorRegistry, static_cast<u32>(i), editorId);
        const auto it = fileMeshByIndex.find(static_cast<u32>(i));
        ecs::Mesh* mesh = editorRegistry.get<ecs::Mesh>(editorId);
        if (it == fileMeshByIndex.end() || mesh == nullptr) continue;
        const ecs::Mesh& src = *it->second;
        if (src.aabb_max.x < src.aabb_min.x || src.aabb_max.y < src.aabb_min.y || src.aabb_max.z < src.aabb_min.z) continue;
        mesh->aabb_min = src.aabb_min;
        mesh->aabb_max = src.aabb_max;
        ++updated;
    }
    return updated;
}
} // namespace fuse::editor
