// E03 hybrid GPU frame: see include/fuse/hybrid/hybrid_scene_renderer.hpp.
#if defined(FUSE_HAS_VULKAN_RHI)

#include <fuse/hybrid/hybrid_scene_renderer.hpp>

#include <fuse/ecs/registry.hpp>
#include <fuse/frame/frame_ctx.hpp>
#include <fuse/renderer/material/material.hpp>
#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/scene_renderer/procedural_meshes.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>
#include <fuse/renderer/vk/present_path.hpp>
#include <fuse/world3d/world_3d.hpp>

#include <cmath>
#include <string>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::hybrid {

namespace sr = renderer::scene_renderer;
namespace fr = renderer::frame;

namespace {

constexpr u32 kDefaultWidth = kPlaceholderWidth;
constexpr u32 kDefaultHeight = kPlaceholderHeight;
constexpr u32 kRgba8 = 37u; // VK_FORMAT_R8G8B8A8_UNORM
constexpr u64 kAllCommands = 0x10000ull;

struct PaletteEntry {
    f32 r, g, b, metallic, roughness;
};
/// Default material rows 0..7 (worlds override them with World3D::setMaterial).
constexpr PaletteEntry kPalette[] = {
    {0.60f, 0.60f, 0.55f, 0.0f, 0.80f}, // 0 floor grey
    {0.80f, 0.30f, 0.20f, 0.2f, 0.35f}, // 1 brick red
    {0.25f, 0.60f, 0.30f, 0.0f, 0.50f}, // 2 green
    {0.20f, 0.35f, 0.80f, 0.0f, 0.40f}, // 3 blue
    {0.90f, 0.70f, 0.30f, 1.0f, 0.30f}, // 4 gold
    {0.90f, 0.90f, 0.90f, 0.0f, 0.60f}, // 5 white
    {0.10f, 0.10f, 0.12f, 0.0f, 0.70f}, // 6 charcoal
    {0.70f, 0.50f, 0.80f, 0.0f, 0.45f}, // 7 lilac
};

renderer::Material toMaterial(const world3d::RenderMaterial3D& m) {
    renderer::Material out{};
    out.baseColor = math::Vec3{m.baseColor[0], m.baseColor[1], m.baseColor[2]};
    out.metallic = m.metallic;
    out.roughness = m.roughness;
    out.emissiveColor = math::Vec3{m.emissive[0], m.emissive[1], m.emissive[2]};
    out.emissiveIntensity = m.emissiveIntensity;
    return out;
}

} // namespace

fr::FrameComposerDesc runtimeComposerDesc() {
    fr::FrameComposerDesc d{};
    d.clusters = renderer::ClusterDesc{};
    d.vsmClipmap.levels = 12;
    d.vsmClipmap.firstLevelExtent = 4.f;
    d.vsmClipmap.markRadiusTexels = 2.5f;
    d.vsmClipmap.texelsPerPixel = 2.f;
    d.vsmPoolPagesX = 16;
    d.vsmPoolPagesY = 16;
    renderer::DDGIDesc& v = d.ddgiVolume;
    v.grid_origin = {-6.f, 0.25f, -8.f};
    v.probe_spacing = {3.f, 1.5f, 3.f};
    v.grid_dims = {5u, 3u, 5u};
    v.rays_per_probe = 64;
    v.probes_per_frame = 75;
    v.irradiance_res = 6;
    v.depth_res = 8;
    v.hysteresis = 0.9f;
    v.max_ray_distance = 20.f;
    d.maxSplats = 64;
    d.maxSplatEntries = 1u << 14;
    d.frameGen = false;
    return d;
}

fr::FrameSettings runtimeFrameSettings() {
    fr::FrameSettings fs{};
    fs.vsmFilter.pcfRadius = 1;
    fs.ssfxSettings.ssgi_params.sample_sqrt = 2;
    fs.atmosphere.sizes.transWidth = 64;
    fs.atmosphere.sizes.transHeight = 32;
    fs.atmosphere.sizes.skyWidth = 64;
    fs.atmosphere.sizes.skyHeight = 48;
    fs.atmosphere.sizes.apWidth = 16;
    fs.atmosphere.sizes.apHeight = 16;
    fs.atmosphere.sizes.apDepth = 16;
    fs.atmosphere.sampling.transSteps = 32;
    fs.atmosphere.sampling.msDirSqrt = 4;
    fs.atmosphere.sampling.skySteps = 16;
    fs.fogSettings.gridX = 24;
    fs.fogSettings.gridY = 16;
    fs.fogSettings.gridZ = 24;
    fs.fogSettings.farPlane = 30.f;
    fs.fogSettings.medium.density = 0.02f;
    fs.clouds = false;
    fs.splats = false;
    fs.restir = false;
    fs.frameGen = false;
    fs.upscaler = fr::FrameUpscaler::None;
    fs.ui = true;
    return fs;
}

HybridSceneRenderer::HybridSceneRenderer() = default;

HybridSceneRenderer::~HybridSceneRenderer() { shutdown(); }

bool HybridSceneRenderer::initialize(renderer::VulkanDevice& device, const HybridSceneRendererDesc& desc) {
    shutdown();
    auto fail = [&](const char* why) {
        shutdown();
        m_reason = why;
        return false;
    };
#if !defined(FUSE_VULKAN_BACKEND)
    (void)device;
    (void)desc;
    return fail("stub backend (no Vulkan)");
#else
    if (!device.isValid() || device.nativeHandle() == nullptr) {
        return fail("no Vulkan device");
    }
    m_device = &device;
    m_desc = desc;
    m_width = desc.width != 0u ? desc.width : kDefaultWidth;
    m_height = desc.height != 0u ? desc.height : kDefaultHeight;

    m_allocator = renderer::GpuAllocator::create(device);
    if (m_allocator == nullptr || !m_allocator->isValid() || m_allocator->isStub()) {
        return fail("GpuAllocator");
    }
    m_bindless = std::make_unique<renderer::BindlessDescriptors>();
    renderer::BindlessDesc bdesc{};
    bdesc.backend = desc.bindlessBackend;
    if (!m_bindless->init(device, bdesc)) {
        return fail("bindless descriptors");
    }
    renderer::rg::ExecutorDesc ed{};
    ed.framesInFlight = 2;
    ed.name = "fuse.hybrid";
    m_executor = renderer::rg::Executor::create(device, m_allocator.get(), ed);
    if (m_executor == nullptr || !m_executor->isValid()) {
        return fail("rg::Executor");
    }

    sr::SceneRendererDesc sd{};
    sd.device = &device;
    sd.allocator = m_allocator.get();
    sd.bindless = m_bindless.get();
    sd.width = m_width;
    sd.height = m_height;
    sd.renderWidth = desc.renderWidth;
    sd.renderHeight = desc.renderHeight;
    sd.tier = desc.tier;
    sd.instanceCapacity = desc.instanceCapacity;
    sd.meshCapacity = desc.meshCapacity;
    sd.materialCapacity = desc.materialCapacity;
    sd.lightCapacity = desc.lightCapacity;
    sd.entityCapacity = desc.entityCapacity;
    sd.composer = desc.runtimeDefaults ? runtimeComposerDesc() : desc.composer;
    m_scene = std::make_unique<sr::SceneRenderer>();
    if (!m_scene->initialize(sd)) {
        m_reason = m_scene->reason();
        const char* why = m_reason;
        shutdown();
        m_reason = why;
        return false;
    }

    // Builtin meshes (world3d::BuiltinMesh) so scenes without cooked assets render.
    std::string error;
    sr::ProceduralMeshDesc cube{};
    cube.shape = sr::ProceduralShape::Cube;
    cube.halfExtent = 1.f;
    sr::ProceduralMeshDesc sphere{};
    sphere.shape = sr::ProceduralShape::Sphere;
    sphere.radius = 1.f;
    sphere.segments = 24;
    sphere.rings = 16;
    sr::ProceduralMeshDesc plane{};
    plane.shape = sr::ProceduralShape::Plane;
    plane.size = 2.f;
    plane.segments = 4;
    if (!m_scene->meshes().registerProcedural(static_cast<u32>(world3d::BuiltinMesh::Cube), cube, &error) ||
        !m_scene->meshes().registerProcedural(static_cast<u32>(world3d::BuiltinMesh::Sphere), sphere, &error) ||
        !m_scene->meshes().registerProcedural(static_cast<u32>(world3d::BuiltinMesh::Plane), plane, &error)) {
        return fail("builtin procedural meshes");
    }
    u32 paletteRow = 0;
    for (const PaletteEntry& p : kPalette) {
        if (paletteRow >= desc.materialCapacity) {
            break;
        }
        renderer::Material m{};
        m.baseColor = math::Vec3{p.r, p.g, p.b};
        m.metallic = p.metallic;
        m.roughness = p.roughness;
        (void)m_scene->setMaterial(paletteRow++, m);
    }
    m_scene->setFrameSettings(desc.runtimeDefaults ? runtimeFrameSettings() : desc.settings);
    (void)m_scene->setUiSource(fr::FrameUiSource{&HybridSceneRenderer::recordUi, this});

    SpriteLayerGpuDesc sl{};
    sl.device = &device;
    sl.allocator = m_allocator.get();
    sl.framesInFlight = 3;
    sl.maxQuads = desc.maxQuads != 0u ? desc.maxQuads : 1u;
    if (!m_sprites.init(sl)) {
        return fail("hybrid.sprite_layer kernel");
    }
    m_overlay.assign(sl.maxQuads, SpriteQuad{});
    m_uiScratch.assign(sl.maxQuads, SpriteQuad{});

    if (!createTargets()) {
        return fail("headless target / readback buffer");
    }
    m_emptyRegistryStorage = std::make_unique<ecs::Registry>();
    m_emptyRegistryStorage->init(16);
    m_emptyRegistry = m_emptyRegistryStorage.get();

    m_valid = true;
    m_reason = "ok";
    return true;
#endif
}

bool HybridSceneRenderer::createTargets() {
    renderer::TextureDesc td{};
    td.width = m_width;
    td.height = m_height;
    td.format = renderer::GpuFormat::R8G8B8A8Unorm;
    // Sampled: the allocator creates a default view.
    td.usage = static_cast<renderer::ImageUsage>(static_cast<u32>(renderer::ImageUsage::TransferDst) |
                                                 static_cast<u32>(renderer::ImageUsage::TransferSrc) |
                                                 static_cast<u32>(renderer::ImageUsage::Sampled));
    td.name = "hybrid.headless_target";
    if (!m_allocator->createImage(td, m_headless)) {
        return false;
    }
    m_headlessLayout = 0;
    m_headlessQueue = rg::kNoQueue;
    if (m_desc.headlessReadback) {
        renderer::BufferDesc bd{};
        bd.size = static_cast<usize>(m_width) * m_height * 4u;
        bd.usage = renderer::BufferUsage::TransferDst;
        bd.memoryUsage = renderer::MemoryUsage::GpuToCpu;
        bd.name = "hybrid.headless_readback";
        if (!m_allocator->createBuffer(bd, m_readback) || m_readback.mapped == nullptr) {
            return false;
        }
    }
    m_readbackFrames = 0;
    return true;
}

void HybridSceneRenderer::destroyTargets() {
    if (m_allocator == nullptr) {
        return;
    }
    if (m_headless.image != nullptr) {
        m_allocator->destroyImage(m_headless);
    }
    if (m_readback.handle != nullptr) {
        m_allocator->destroyBuffer(m_readback);
    }
    m_headless = renderer::Texture{};
    m_readback = renderer::Buffer{};
}

void HybridSceneRenderer::shutdown() {
    if (m_executor != nullptr) {
        (void)m_executor->waitIdle();
    }
    if (m_scene != nullptr) {
        if (m_scene->valid()) {
            (void)m_scene->upload().waitAll();
        }
        m_scene.reset();
    }
    m_sprites.destroy();
    destroyTargets();
    m_executor.reset();
    if (m_bindless != nullptr && m_device != nullptr) {
        m_bindless->collectRetired(~0ull);
        m_bindless->destroy(*m_device);
    }
    m_bindless.reset();
    m_allocator.reset();
    m_emptyRegistryStorage.reset();
    m_emptyRegistry = nullptr;
    m_overlay.clear();
    m_uiScratch.clear();
    m_spriteCount = 0;
    m_uiCount = 0;
    m_outputs = fr::FrameGraphOutputs{};
    m_recorded = false;
    m_readbackFrames = 0;
    m_materialVersion = ~0ull;
    m_materialWorld = nullptr;
    m_framesSubmitted = 0;
    m_stats = HybridSceneFrameStats{};
    m_valid = false;
    m_device = nullptr;
    m_reason = "not initialised";
}

bool HybridSceneRenderer::resize(u32 width, u32 height) {
    if (!m_valid || width == 0u || height == 0u) {
        return false;
    }
    if (width == m_width && height == m_height) {
        return true;
    }
    (void)m_executor->waitIdle();
    if (!m_scene->resize(width, height, m_desc.renderWidth, m_desc.renderHeight)) {
        return false;
    }
    destroyTargets();
    m_width = width;
    m_height = height;
    m_recorded = false;
    return createTargets();
}

void HybridSceneRenderer::beginOverlay() {
    m_spriteCount = 0;
    m_uiCount = 0;
}

bool HybridSceneRenderer::addSprite(const SpriteQuad& quad) {
    // Sprites precede the UI quads in m_overlay; UI quads wait in m_uiScratch until recordUi joins them.
    if (m_spriteCount + m_uiCount >= m_overlay.size()) {
        return false;
    }
    m_overlay[m_spriteCount++] = quad;
    return true;
}

bool HybridSceneRenderer::addUi(const SpriteQuad& quad) {
    if (m_spriteCount + m_uiCount >= m_uiScratch.size()) {
        return false;
    }
    m_uiScratch[m_uiCount++] = quad;
    return true;
}

rg::TextureRef HybridSceneRenderer::recordUi(rg::Graph& graph, const fr::FrameUiContext& context, void* user) {
    HybridSceneRenderer& self = *static_cast<HybridSceneRenderer*>(user);
    const u32 count = self.m_spriteCount + self.m_uiCount;
    if (count == 0u || !self.m_sprites.valid()) {
        return {};
    }
    for (u32 i = 0; i < self.m_uiCount; ++i) {
        self.m_overlay[self.m_spriteCount + i] = self.m_uiScratch[i];
    }
    self.m_sprites.beginFrame(context.serial);
    return self.m_sprites.addPass(graph, self.m_overlay.data(), count, context.width, context.height);
}

void HybridSceneRenderer::syncMaterials(const world3d::World3D& world) {
    if (&world == m_materialWorld && world.materialVersion() == m_materialVersion) {
        return;
    }
    const std::vector<world3d::RenderMaterial3D>& materials = world.materials();
    for (u32 i = 0; i < materials.size() && i < m_desc.materialCapacity; ++i) {
        (void)m_scene->setMaterial(i, toMaterial(materials[i]));
    }
    m_materialWorld = &world;
    m_materialVersion = world.materialVersion();
}

bool HybridSceneRenderer::renderWorld3D(world3d::World3D& world, frame::FrameCtx& ctx) {
    if (!m_valid) {
        return false;
    }
    syncMaterials(world);
    ecs::Registry& registry = world.registry();
    fr::FrameCamera camera{};
    const world3d::RenderCamera3D& wc = world.camera();
    if (wc.valid) {
        for (u32 a = 0; a < 3u; ++a) {
            camera.eye[a] = wc.eye[a];
            camera.target[a] = wc.target[a];
        }
        camera.fovY = wc.fovY;
        camera.nearPlane = wc.nearPlane;
        camera.farPlane = wc.farPlane;
    } else if (!sr::cameraFromRegistry(registry, camera)) {
        camera = fr::FrameCamera{};
    }
    return renderRegistry(registry, camera, ctx);
}

bool HybridSceneRenderer::renderEmpty(frame::FrameCtx& ctx) {
    if (!m_valid) {
        return false;
    }
    return renderRegistry(*m_emptyRegistry, fr::FrameCamera{}, ctx);
}

bool HybridSceneRenderer::renderRegistry(ecs::Registry& registry, const fr::FrameCamera& camera, frame::FrameCtx& ctx) {
    if (!m_valid) {
        return false;
    }
    const u64 serial = m_scene->frameSerial() + 1u;
    // The executor ring is 2 deep: submitting frame serial - 1 waited for frame serial - 3.
    if (serial > 3u) {
        m_scene->collectRetired(serial - 3u);
    }
    m_graph.reset();
    sr::SceneFrameDesc sfd{};
    sfd.serial = serial;
    sfd.deltaSeconds = ctx.dt > 0.f ? ctx.dt : 1.f / 60.f;
    m_outputs = m_scene->renderScene(registry, camera, m_graph, sfd);
    m_recorded = m_outputs.output.valid();
    m_stats = HybridSceneFrameStats{};
    m_stats.serial = serial;
    m_stats.sprites = m_spriteCount;
    m_stats.uiQuads = m_uiCount;
    m_stats.instances = m_scene->scene().liveInstances();
    m_stats.lights = m_scene->lastFrame().extract.lights;
    m_stats.uiStageRan = (m_scene->composer().stats().ran & fr::kStageUi) != 0u;
    return m_recorded;
}

bool HybridSceneRenderer::submitThunk(const renderer::SceneFrameSubmit& submit, void* user) {
    return static_cast<HybridSceneRenderer*>(user)->submitFrame(submit);
}

bool HybridSceneRenderer::submitFrame(const renderer::SceneFrameSubmit& submit) {
    if (!m_valid || !m_recorded) {
        return false;
    }
    return execute(submit.waitSemaphore, submit.signalSemaphore, submit.fence,
                   submit.swapchainImage != nullptr ? &submit : nullptr);
}

bool HybridSceneRenderer::submitHeadless() {
    if (!m_valid || !m_recorded) {
        return false;
    }
    return execute(nullptr, nullptr, nullptr, nullptr);
}

bool HybridSceneRenderer::execute(void* waitSemaphore, void* signalSemaphore, void* fence,
                                  const renderer::SceneFrameSubmit* swapchain) {
    sr::ScenePresentTargets targets{};
    renderer::PresentTargetDesc p{};
    if (swapchain != nullptr) {
        p.image = swapchain->swapchainImage;
        p.view = swapchain->swapchainView;
        p.format = swapchain->swapchainFormat;
        p.width = swapchain->swapchainWidth;
        p.height = swapchain->swapchainHeight;
        // Imported UNDEFINED (the acquired image's contents are discarded); addPresent's "present.handoff" leaves it
        // in PRESENT_SRC_KHR, ordered before the renderFinished signal.
        p.swapchain = true;
        p.name = "hybrid.swapchain";
        targets.swapchain = true;
    } else {
        p.image = m_headless.image;
        p.view = m_headless.view;
        p.format = kRgba8;
        p.width = m_width;
        p.height = m_height;
        p.layoutTracker = &m_headlessLayout;
        p.queueTracker = &m_headlessQueue;
        p.name = "hybrid.headless_target";
    }
    targets.target = renderer::importPresentTarget(m_graph, p);
    targets.width = p.width;
    targets.height = p.height;
    if (!m_scene->addPresent(m_graph, m_outputs, targets)) {
        m_recorded = false;
        return false;
    }
    const bool readback = swapchain == nullptr && m_readback.handle != nullptr;
    if (readback) {
        m_copy.src = targets.target;
        m_copy.dst = m_graph.importBuffer(
            rg::ImportedBuffer{m_readback.handle, m_readback.desc.size, rg::kNoQueue, nullptr, "hybrid.headless_readback"});
        m_copy.width = m_width;
        m_copy.height = m_height;
        const u64 bytes = static_cast<u64>(m_width) * m_height * 4u;
        m_graph.addPass("hybrid.readback", &HybridSceneRenderer::recordCopy, &m_copy)
            .use(m_copy.src, rg::Access::TransferSrc)
            .use(m_copy.dst, rg::Access::TransferDst, rg::BufferRange{0, bytes});
        m_graph.addPass("hybrid.readback.host", nullptr, nullptr).use(m_copy.dst, rg::Access::HostRead);
    }
    renderer::rg::SubmitDesc sd{};
    sd.waitSemaphore = waitSemaphore;
    sd.waitStages = kAllCommands;
    sd.signalSemaphore = signalSemaphore;
    sd.fence = fence;
    const renderer::rg::ExecuteResult result = m_executor->execute(m_graph, sd);
    m_recorded = false;
    if (!result.ok) {
        return false;
    }
    ++m_framesSubmitted;
    if (readback) {
        ++m_readbackFrames;
    }
    m_stats.submitted = true;
    m_stats.presentedToSwapchain = swapchain != nullptr;
    m_stats.headless = swapchain == nullptr;
    return true;
}

void HybridSceneRenderer::recordCopy(const rg::PassContext& pc, void* user) {
#if defined(FUSE_VULKAN_BACKEND)
    const Copy& c = *static_cast<const Copy*>(user);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {c.width, c.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkImage>(pc.image(c.src)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(pc.buffer(c.dst)), 1, &region);
#else
    (void)pc;
    (void)user;
#endif
}

bool HybridSceneRenderer::waitIdle() {
    if (m_executor == nullptr) {
        return true;
    }
    const bool ok = m_executor->waitIdle();
    if (m_scene != nullptr && m_scene->valid()) {
        (void)m_scene->upload().waitAll();
        m_scene->collectRetired(m_scene->frameSerial());
    }
    return ok;
}

const u8* HybridSceneRenderer::readbackPixels() const {
    return m_readbackFrames != 0u && m_readback.mapped != nullptr ? static_cast<const u8*>(m_readback.mapped) : nullptr;
}

OverlayReadbackCheck checkOverlayReadback(const HybridSceneRenderer& renderer, u32 tolerance) {
    OverlayReadbackCheck out{};
    const u8* px = renderer.readbackPixels();
    if (px == nullptr) {
        return out;
    }
    out.valid = true;
    const u32 w = renderer.width();
    const u32 h = renderer.height();
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            f32 ref[4];
            spriteLayerTexel(renderer.overlayQuads(), renderer.overlayCount(), x, y, ref);
            if (ref[3] < 1.f) {
                continue;
            }
            ++out.covered;
            const u8* p = px + (static_cast<usize>(y) * w + x) * 4u;
            for (u32 c = 0; c < 3u; ++c) {
                const long want = std::lround(ref[c] * 255.f);
                const long diff = want > p[c] ? want - p[c] : p[c] - want;
                out.mismatched += diff <= static_cast<long>(tolerance) ? 0u : 1u;
            }
        }
    }
    return out;
}

bool readbackPixel(const HybridSceneRenderer& renderer, u32 x, u32 y, u8 rgb[3]) {
    const u8* px = renderer.readbackPixels();
    if (px == nullptr || x >= renderer.width() || y >= renderer.height()) {
        return false;
    }
    const u8* p = px + (static_cast<usize>(y) * renderer.width() + x) * 4u;
    rgb[0] = p[0];
    rgb[1] = p[1];
    rgb[2] = p[2];
    return true;
}

} // namespace fuse::hybrid

#endif // defined(FUSE_HAS_VULKAN_RHI)
