#include <fuse/editor/runtime_viewport.hpp>

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/editor/viewport_present_gate.hpp>
#include <fuse/editor/viewport_swapchain_recreate.hpp>
#include <fuse/editor/viewport_swapchain_wiring.hpp>
#include <fuse/editor/viewport_vulkan_surface.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/platform/window_wsi.hpp>
#include <fuse/jobs/worker_count.hpp>
#include <fuse/io/vfs.hpp>
#include <fuse/hybrid/project_flags.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/project/manifest.hpp>
#include <fuse/project/world_converter.hpp>
#include <fuse/project/t3d_asset_vfs.hpp>
#include <fuse/world2d/world_2d.hpp>
#include <fuse/world3d/world_3d.hpp>
#include <fuse/scene/project_io.hpp>
#include <fuse/scene/serialiser.hpp>
#include <fuse/scene/wire_runtime_bind.hpp>

#include <vector>

#include <algorithm>
#include <cmath>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_renderer_bootstrap.hpp>
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

void applyEcsTransformToSceneObject3D(const ecs::Transform& transform, fuse::SceneObject3D& object) {
    object.setPosition(transform.position.x, transform.position.y);
    object.setZ(transform.position.z);
    const EulerDeg euler = quatToEulerDeg(transform.rotation);
    object.setYawDeg(euler.yaw);
    object.setPitchDeg(euler.pitch);
    object.setRollDeg(euler.roll);
}

} // namespace

#if defined(FUSE_VULKAN_BACKEND)
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

void attachEmbedWorldsToHybrid(RuntimeViewportHeadlessGpuStub* gpu, RuntimeEmbedSession& session,
                               const fuse::project::ProjectManifest& manifest) {
    if (gpu == nullptr || gpu->hybrid == nullptr || gpu->worldsAttached) {
        return;
    }

    fuse::hybrid::DimensionFlags flags = fuse::project::toDimensionFlags(manifest.dimensions);
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
        if (gpu != nullptr) {
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
    const fuse::scene::WireRuntimeBindResult wireBindings =
        fuse::scene::applyWireBindingsFromScene(host.editorScene().registry(), runtimeScene);
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
    RuntimeViewportHeadlessGpuStub* gpu = asHeadlessGpuStub(m_headlessGpuStub);
    if (gpu != nullptr) {
        attachEmbedWorldsToHybrid(gpu, m_embedSession, projectLoad.manifest);
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
    m_materialAssetTable.commit();
    if (fuse::io::VirtualFileSystem::instance().completedLoadCount() == 0u) {
        m_materialLoadsPending = false;
    }
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
}

void RuntimeViewportHook::mirrorEditorEntities_(EditorHost& host) {
    std::vector<ecs::EntityID> entityOrder;
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

    std::vector<ecs::EntityID> entityOrder;
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
    bootstrapDesc.renderer.rhi.bootstrap.swapchain = hook.buildSwapchainDescHandoff();

    gpu->hybrid = fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
    if (gpu->hybrid != nullptr && gpu->hybrid->isReady()) {
        hook.embedSession().wsiPresentPathReady = true;
        hook.embedSession().qtLivePresentReady = true;
        hook.embedSession().headlessGpuReady = true;
    }
}
#endif

void RuntimeViewportHook::tickHeadlessPresentStub_(EditorHost& host, f32 dt) {
    if (!m_embedded) {
        return;
    }

    m_embedSession.wsiBackendName = fuse::platform::windowWsiBackendName();
    m_embedSession.usesHeadlessGpuPath =
        fuse::platform::activeWindowWsiKind() == fuse::platform::WindowWsiKind::Null;
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
        if (m_surfaceHandoff.nativeSurface != nullptr) {
            bootstrapDesc.presentable.swapchainWidth = m_surfaceHandoff.width;
            bootstrapDesc.presentable.swapchainHeight = m_surfaceHandoff.height;
            bootstrapDesc.renderer.rhi.bootstrap.swapchain = buildSwapchainDescHandoff();
        }
        gpu->hybrid = fuse::hybrid::HybridRendererBootstrap::create(bootstrapDesc);
        if (gpu->hybrid != nullptr && gpu->hybrid->isReady()) {
            m_embedSession.wsiPresentPathReady = true;
            m_embedSession.headlessGpuReady = true;
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
        const u32 composerFramesBefore = gpu->hybrid->composer().frameCount();
        gpu->hybrid->runFrame(frameCtx);
        if (gpu->hybrid->composer().frameCount() > composerFramesBefore) {
            ++m_embedSession.hybridComposerFrames;
            m_embedSession.ecsWorld3DSnapshotVisible = gpu->embedWorld3D.readSnapshot().visibleCount();
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

#else
    (void)host;
    (void)dt;
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

    if (!m_lastProjectLabel.empty()) {
        if (host.runtimeScene().name() != m_lastProjectLabel) {
            host.runtimeScene().setName(m_lastProjectLabel);
        }
        m_embedded = true;
    }

    m_panel.tick(dt);

#if defined(FUSE_VULKAN_BACKEND)
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

    tickHeadlessPresentStub_(host, dt);
    ++m_runtimeTickCount;
}

} // namespace fuse::editor
