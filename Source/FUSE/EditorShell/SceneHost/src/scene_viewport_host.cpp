#include "fuse/editor/scene_viewport_host.hpp"

#include <fuse/editor/editor_host.hpp>
#include <fuse/editor/viewport_panel.hpp>

#if defined(FUSE_HAS_VULKAN_RHI)
#include <fuse/ecs/systems/transform_system.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/hybrid/hybrid_scene_renderer.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/instance.hpp>
#include <fuse/world3d/world_3d.hpp>
#endif

#include <algorithm>
#include <cstring>

namespace fuse::editor {

struct SceneViewportHost::Impl {
    std::string reason = "not initialised";
    bool readback = true;
    u64 frames = 0;
    u32 width = 0, height = 0;
#if defined(FUSE_HAS_VULKAN_RHI)
    std::unique_ptr<renderer::VulkanInstance> instance;
    std::unique_ptr<renderer::VulkanDevice> device;
    std::unique_ptr<hybrid::HybridSceneRenderer> gpu;
    world3d::World3D world;
    u32 materialRevision = ~0u;
    f32 time = 0.f;
#endif
};

SceneViewportHost::SceneViewportHost() : m(std::make_unique<Impl>()) {}

SceneViewportHost::~SceneViewportHost() {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m->gpu) m->gpu->shutdown();
    m->gpu.reset();
    m->device.reset();
    m->instance.reset();
#endif
}

std::unique_ptr<SceneViewportHost> SceneViewportHost::create(const SceneViewportHostDesc& desc) {
    std::unique_ptr<SceneViewportHost> h(new SceneViewportHost());
    Impl& m = *h->m;
    m.readback = desc.readback;
#if defined(FUSE_HAS_VULKAN_RHI)
    // surface extensions so a swapchain presenter can adopt the device (skipped by the loader when missing)
    const char* exts[] = {"VK_KHR_surface",
#if defined(_WIN32)
                          "VK_KHR_win32_surface"
#else
                          "VK_KHR_xlib_surface"
#endif
    };
    renderer::VulkanInstanceDesc id{};
    id.appName = "fuse_editor_scene_host";
    id.enableValidation = desc.validation;
    id.extraExtensions = exts;
    id.extraExtensionCount = 2;
    m.instance = renderer::VulkanInstance::create(id);
    if (!m.instance || !m.instance->isValid()) {
        m.reason = "no Vulkan instance" + (m.instance ? ": " + m.instance->info().message : std::string());
        return h;
    }
    renderer::VulkanDeviceDesc dd{};
    dd.preferDiscreteGpu = true;
    m.device = renderer::VulkanDevice::create(*m.instance, dd);
    if (!m.device || !m.device->isValid()) {
        m.reason = "no Vulkan device" + (m.device ? ": " + m.device->info().message : std::string());
        return h;
    }
    m.gpu = std::make_unique<hybrid::HybridSceneRenderer>();
    hybrid::HybridSceneRendererDesc sd{};
    sd.width = 640;
    sd.height = 360;
    sd.sdfProxyMeshes = true; // SDF objects show as sphere / cube proxies, as in the Qt viewport
    sd.headlessReadback = desc.readback;
    if (!m.gpu->initialize(*m.device, sd)) {
        m.reason = std::string("scene renderer: ") + m.gpu->reason();
        m.gpu.reset();
        return h;
    }
    m.width = sd.width;
    m.height = sd.height;
    m.world.setClearColor(0.08f, 0.12f, 0.18f);
    m.reason.clear();
#else
    (void)desc;
    m.reason = "built without the Vulkan RHI";
#endif
    return h;
}

bool SceneViewportHost::valid() const {
#if defined(FUSE_HAS_VULKAN_RHI)
    return m->gpu && m->gpu->valid();
#else
    return false;
#endif
}
const std::string& SceneViewportHost::reason() const { return m->reason; }

std::string SceneViewportHost::deviceName() const {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m->device) return m->device->info().deviceName;
#endif
    return {};
}

void* SceneViewportHost::instanceHandle() const {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m->device) return m->device->instanceHandle();
#endif
    return nullptr;
}
void* SceneViewportHost::physicalDeviceHandle() const {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m->device) return m->device->nativePhysicalDevice();
#endif
    return nullptr;
}
void* SceneViewportHost::deviceHandle() const {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m->device) return m->device->nativeHandle();
#endif
    return nullptr;
}
void* SceneViewportHost::queueHandle() const {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m->device) return m->device->queues().graphics;
#endif
    return nullptr;
}
u32 SceneViewportHost::queueFamily() const {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m->device) return m->device->queues().graphicsFamily;
#endif
    return 0;
}

bool SceneViewportHost::render(EditorHost& host, const ViewportPanel& view, u32 width, u32 height, f32 dt) {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (!valid() || width == 0 || height == 0) return false;
    hybrid::HybridSceneRenderer& gpu = *m->gpu;
    if (width != gpu.width() || height != gpu.height()) {
        if (!gpu.resize(width, height)) {
            m->reason = std::string("resize: ") + gpu.reason();
            return false;
        }
    }
    m->width = width;
    m->height = height;
    // same scene feed as RuntimeViewportHook::syncEcsToEmbedWorld3D_ (the Qt viewport): world matrices current, the
    // editor registry rendered directly, the viewport fly camera, the material catalog as World3D's table
    ecs::Registry& registry = host.editorScene().registry();
    ecs::TransformSystem::update(registry);
    m->world.setRenderRegistry(&registry);
    const ViewportCamera& cam = view.camera();
    const ecs::vec3 fwd = view.forward();
    world3d::RenderCamera3D rc{};
    rc.eye[0] = cam.positionX;
    rc.eye[1] = cam.positionY;
    rc.eye[2] = cam.positionZ;
    rc.target[0] = cam.positionX + fwd.x;
    rc.target[1] = cam.positionY + fwd.y;
    rc.target[2] = cam.positionZ + fwd.z;
    rc.fovY = cam.fovDeg * 0.017453292519943295f;
    rc.nearPlane = cam.nearPlane > 0.f ? cam.nearPlane : 0.1f;
    rc.farPlane = (std::min)(cam.farPlane > rc.nearPlane ? cam.farPlane : 1000.f, 2000.f);
    rc.valid = true;
    m->world.setCamera(rc);
    if (host.materials().revision() != m->materialRevision) {
        m->materialRevision = host.materials().revision();
        host.materials().applyTo(m->world);
    }
    frame::FrameCtx ctx{};
    ctx.dt = dt;
    ctx.frameIndex = u32(m->frames + 1);
    m->time += dt;
    ctx.time = m->time;
    gpu.beginOverlay();
    if (!gpu.renderWorld3D(m->world, ctx) || !gpu.submitHeadless()) {
        m->reason = std::string("frame: ") + gpu.reason();
        return false;
    }
    ++m->frames;
    return true;
#else
    (void)host, (void)view, (void)width, (void)height, (void)dt;
    return false;
#endif
}

SceneViewportFrame SceneViewportHost::frame() const {
    SceneViewportFrame f{};
#if defined(FUSE_HAS_VULKAN_RHI)
    if (!valid() || m->frames == 0) return f;
    const hybrid::HybridSceneRenderer::HeadlessTarget t = m->gpu->headlessTarget();
    f.image = t.image;
    f.view = t.view;
    f.layout = t.layout;
    f.width = m->gpu->width();
    f.height = m->gpu->height();
    f.serial = m->frames;
#endif
    return f;
}

u64 SceneViewportHost::framesRendered() const { return m->frames; }

bool SceneViewportHost::readback(std::vector<u32>& out, u32& width, u32& height) {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (!valid() || !m->readback || m->frames == 0) return false;
    if (!m->gpu->waitIdle()) return false;
    const u8* px = m->gpu->readbackPixels();
    if (!px) return false;
    width = m->gpu->width();
    height = m->gpu->height();
    out.resize(std::size_t(width) * height);
    std::memcpy(out.data(), px, out.size() * 4);
    for (u32& p : out) p |= 0xFF000000u; // the scene frame is opaque
    return true;
#else
    (void)out, (void)width, (void)height;
    return false;
#endif
}

void SceneViewportHost::waitIdle() {
#if defined(FUSE_HAS_VULKAN_RHI)
    if (m->gpu) m->gpu->waitIdle();
    else if (m->device) m->device->waitIdle();
#endif
}

} // namespace fuse::editor
