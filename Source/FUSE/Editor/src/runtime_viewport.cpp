#include <fuse/editor/runtime_viewport.hpp>

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/editor_scene.hpp>
#include <fuse/ecs/components/transform.hpp>
#include <fuse/log/logger.hpp>
#include <fuse/project/loader.hpp>
#include <fuse/scene/project_io.hpp>
#include <fuse/scene/serialiser.hpp>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/rhi_context.hpp>
#include <fuse/renderer/render_command_list.hpp>
#endif

namespace fuse::editor {

#if defined(FUSE_VULKAN_BACKEND)
struct RuntimeViewportHook::HeadlessGpuStub {
    std::unique_ptr<fuse::renderer::RhiContext> context;
    u32 submittedFrames = 0;
};
#endif

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
}

void RuntimeViewportHook::setExternalSurfaceHandle(void* vkSurface, u32 width, u32 height) {
    m_surfaceHandoff.nativeSurface = vkSurface;
    m_surfaceHandoff.width = width > 0 ? width : m_panel.width();
    m_surfaceHandoff.height = height > 0 ? height : m_panel.height();
    m_surfaceHandoff.pending = vkSurface != nullptr;
    m_surfaceHandoff.consumed = false;

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

    fuse::scene::Scene& runtimeScene = host.runtimeScene();
    const fuse::scene::SerialiseResult loaded =
        fuse::scene::loadForProject(runtimeScene, projectLoad);
    if (loaded.status != fuse::scene::SerialiseStatus::Ok) {
        fuse::log::warn("RuntimeViewportHook: world load failed: %s", loaded.error.c_str());
        return;
    }

    m_embedSession.loadedWorldPath = fuse::scene::resolveDefaultWorldPath(projectLoad.manifest);
    m_embedSession.worldEntityCount = runtimeScene.entityCount();
    m_embedSession.worldLoaded = true;
    m_embedded = true;

    if (!m_lastProjectLabel.empty()) {
        runtimeScene.setName(m_lastProjectLabel);
    }
}

void RuntimeViewportHook::mirrorEditorEntities_(EditorHost& host) {
    u32 aliveCount = 0;
    host.editorScene().registry().each_query<ecs::Transform>(
        [&](ecs::EntityID /*id*/, ecs::Transform& /*transform*/) { ++aliveCount; });

    if (aliveCount == m_embedSession.mirroredEditorEntityCount) {
        return;
    }

    if (aliveCount == 0) {
        m_embedSession.mirroredEditorEntityCount = 0;
        return;
    }

    fuse::scene::Scene& runtimeScene = host.runtimeScene();
    runtimeScene.clearEntities();
    u32 index = 0;
    host.editorScene().registry().each_query<ecs::Transform>([&](ecs::EntityID id, ecs::Transform& transform) {
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
            parentIndex = 0;
        }

        const std::string name = "EditorEntity_" + std::to_string(index++);
        runtimeScene.addEntity(name, sceneTransform, parentIndex);
    });

    m_embedSession.mirroredEditorEntityCount = aliveCount;
    m_embedded = true;
}

void RuntimeViewportHook::tickHeadlessPresentStub_(EditorHost& host, f32 /*dt*/) {
    if (!m_embedded) {
        return;
    }

    ++m_embedSession.headlessPresentTicks;

#if defined(FUSE_VULKAN_BACKEND)
    if (!m_headlessGpu) {
        m_headlessGpu = std::make_unique<HeadlessGpuStub>();
    }

    if (!m_embedSession.headlessGpuReady && m_headlessGpu->context == nullptr) {
        fuse::renderer::RhiContext::Desc desc{};
        desc.bootstrap.instance.enableValidation = false;
        desc.enableRasterPath = false;
        desc.enableCompositePass = false;
        m_headlessGpu->context = fuse::renderer::RhiContext::create(desc);
        if (m_headlessGpu->context != nullptr &&
            m_headlessGpu->context->bootstrap().status().deviceReady) {
            m_embedSession.headlessGpuReady = true;
        }
    }

    if (m_embedSession.headlessGpuReady && m_headlessGpu->context != nullptr) {
        fuse::renderer::RenderCommandList commands;
        if (m_headlessGpu->context->beginFrame(0) &&
            m_headlessGpu->context->submitFrame(commands, 0)) {
            ++m_headlessGpu->submittedFrames;
        }
    }

#else
    (void)host;
#endif
}

void RuntimeViewportHook::tick(EditorHost& host, f32 dt) {
    applyPendingResize_();
    ensureWorldLoaded_(host);
    mirrorEditorEntities_(host);

    if (!m_lastProjectLabel.empty()) {
        if (host.runtimeScene().name() != m_lastProjectLabel) {
            host.runtimeScene().setName(m_lastProjectLabel);
        }
        m_embedded = true;
    }

    m_panel.tick(dt);

    if (m_surfaceHandoff.pending) {
        m_surfaceHandoff.pending = false;
        m_surfaceHandoff.consumed = true;
        m_embedSession.surfaceHandoffPending = false;
        m_embedSession.surfaceHandoffConsumed = true;
    }

    tickHeadlessPresentStub_(host, dt);
    ++m_runtimeTickCount;
}

} // namespace fuse::editor
