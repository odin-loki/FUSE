#include "frame_runner.hpp"

#include <fuse/renderer/deferred/gbuffer.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>

#if defined(FUSE_VULKAN_BACKEND)
#include <fuse/renderer/deferred/gbuffer_raster_pass.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/queue_submit.hpp>
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::harness {

using math::Vec3;
using math::Vec4;

namespace {

[[maybe_unused]] f64 msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

u8 encodeSrgb(f32 linear) {
    const f32 c = std::min(std::max(linear, 0.f), 1.f);
    const f32 s = c <= 0.0031308f ? 12.92f * c : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
    return static_cast<u8>(std::floor(s * 255.f + 0.5f));
}

[[maybe_unused]] void setEnvIfUnset(const char* name, const char* value) {
    const char* current = std::getenv(name);
    if (current != nullptr && current[0] != '\0') {
        return;
    }
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

[[maybe_unused]] bool envDisabled(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] == '0' && v[1] == '\0';
}

[[maybe_unused]] bool fileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return static_cast<bool>(f);
}

} // namespace

int parseTier(const std::string& text) {
    std::string t = text;
    if (!t.empty() && (t[0] == 'T' || t[0] == 't')) {
        t = t.substr(1);
    }
    if (t.size() == 1u && t[0] >= '0' && t[0] <= '3') {
        return t[0] - '0';
    }
    return -1;
}

u32 FrameCapture::coveredPixels() const {
    u32 n = 0;
    for (const Vec4& a : albedo) {
        n += a.w > 0.f ? 1u : 0u;
    }
    return n;
}

void resolveCapture(FrameCapture& capture, const Vec3& viewDir, const ResolveParams& params) {
    const usize n = static_cast<usize>(capture.width) * capture.height;
    capture.lit = ImageRgba8(capture.width, capture.height);
    capture.hudMask.assign(n, 0u);
    const Vec3 l = params.lightDir.normalized();
    const Vec3 v = viewDir.normalized();
    const Vec3 h = (l + v).normalized();
    for (usize i = 0; i < n; ++i) {
        Vec3 c = params.background;
        const Vec4& a = capture.albedo[i];
        if (a.w > 0.f) {
            const u32 shading = static_cast<u32>(std::floor(capture.surface[i].w * 255.f + 0.5f));
            if (shading == kHudShadingModel) {
                capture.hudMask[i] = 1u;
                c = capture.emissive[i];
            } else {
                const Vec3& nrm = capture.normal[i];
                const f32 roughness = capture.surface[i].x;
                const f32 metallic = capture.surface[i].y;
                const f32 halfLambert = 0.5f + 0.5f * nrm.dot(l);
                const f32 diffuse = 0.15f + 0.85f * halfLambert * halfLambert;
                const f32 gloss = (1.f - roughness) * (1.f - roughness);
                const f32 spec = 0.3f * gloss * std::pow(std::max(nrm.dot(h), 0.f), 4.f + 60.f * gloss);
                const f32 kd = capture.ao[i] * diffuse * (1.f - 0.5f * metallic);
                c = Vec3(a.x * kd + spec, a.y * kd + spec, a.z * kd + spec) + capture.emissive[i];
            }
        }
        u8* p = capture.lit.pixels.data() + i * 4u;
        p[0] = encodeSrgb(c.x);
        p[1] = encodeSrgb(c.y);
        p[2] = encodeSrgb(c.z);
        p[3] = 255u;
    }
}

bool writeCaptureExr(const std::string& path, const FrameCapture& capture) {
    const usize n = static_cast<usize>(capture.width) * capture.height;
    if (n == 0u || capture.normal.size() != n) {
        return false;
    }
    std::vector<ExrChannel> channels;
    auto add = [&](const char* name, auto&& get) {
        ExrChannel c;
        c.name = name;
        c.data.resize(n);
        for (usize i = 0; i < n; ++i) {
            c.data[i] = get(i);
        }
        channels.push_back(std::move(c));
    };
    add("albedo.R", [&](usize i) { return capture.albedo[i].x; });
    add("albedo.G", [&](usize i) { return capture.albedo[i].y; });
    add("albedo.B", [&](usize i) { return capture.albedo[i].z; });
    add("depth.Z", [&](usize i) { return capture.depth[i]; });
    add("emissive.R", [&](usize i) { return capture.emissive[i].x; });
    add("emissive.G", [&](usize i) { return capture.emissive[i].y; });
    add("emissive.B", [&](usize i) { return capture.emissive[i].z; });
    add("normal.X", [&](usize i) { return capture.normal[i].x; });
    add("normal.Y", [&](usize i) { return capture.normal[i].y; });
    add("normal.Z", [&](usize i) { return capture.normal[i].z; });
    return writeExr(path, capture.width, capture.height, std::move(channels));
}

// ---- Vulkan implementation ---------------------------------------------------------------------

#if defined(FUSE_VULKAN_BACKEND)

struct HeadlessFrameRunner::Impl {
    std::unique_ptr<VulkanBootstrap> bootstrap;
    BindlessDescriptors bindless;
    ResourceManager resources;
    GBuffer gbuffer;
    GBufferRasterPass stockPass;
    GBufferRasterPass projectedPass;
    bool bindlessReady = false;
    bool resourcesReady = false;
    bool gbufferReady = false;
    std::string stockVert;
    std::string frag;
    std::string projectedVert;
    u32 frameCounter = 0;

    ~Impl() {
        if (bootstrap != nullptr && bootstrap->device() != nullptr) {
            bootstrap->device()->waitIdle();
        }
        stockPass.destroy();
        projectedPass.destroy();
        if (gbufferReady) {
            gbuffer.destroy();
        }
        if (resourcesReady) {
            resources.destroy();
        }
        if (bindlessReady && bootstrap != nullptr && bootstrap->device() != nullptr) {
            bindless.destroy(*bootstrap->device());
        }
        bootstrap.reset();
    }

    GBufferRasterPass* pass(RasterShaders shaders, std::string& error) {
        GBufferRasterPass& p = shaders == RasterShaders::Stock ? stockPass : projectedPass;
        if (p.isReady()) {
            return &p;
        }
        const std::string& vert = shaders == RasterShaders::Stock ? stockVert : projectedVert;
        GBufferRasterPassDesc desc{};
        desc.vertexSpirvPath = vert.c_str();
        desc.fragmentSpirvPath = frag.c_str();
        if (!p.init(*bootstrap->device(), resources, gbuffer, desc)) {
            error = "G-buffer raster pass init failed: " + p.stats().message;
            return nullptr;
        }
        return &p;
    }
};

namespace {

TierReport inferTier(VulkanBootstrap& bootstrap, int cap) {
    TierReport report;
    int hardware = 0;
    const auto instance = static_cast<VkInstance>(bootstrap.instance()->nativeHandle());
    u32 count = 0;
    vkEnumeratePhysicalDevices(instance, &count, nullptr);
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance, &count, devices.data());
    const u32 index = bootstrap.device()->info().physicalDeviceIndex;
    if (index < devices.size()) {
        u32 extCount = 0;
        vkEnumerateDeviceExtensionProperties(devices[index], nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateDeviceExtensionProperties(devices[index], nullptr, &extCount, exts.data());
        auto has = [&exts](const char* name) {
            return std::any_of(exts.begin(), exts.end(),
                               [name](const VkExtensionProperties& e) { return std::strcmp(e.extensionName, name) == 0; });
        };
        const bool mesh = has("VK_EXT_mesh_shader");
        const bool rt = has("VK_KHR_acceleration_structure") && has("VK_KHR_ray_query");
        const bool full = has("VK_KHR_ray_tracing_pipeline") && has("VK_KHR_cooperative_matrix");
        hardware = mesh ? (rt ? (full ? 3 : 2) : 1) : 0;
    }
    int effective = hardware;
    if (cap >= 0) {
        effective = std::min(effective, cap);
    }
    const char* envCap = std::getenv("FUSE_RENDER_TIER_MAX");
    if (envCap != nullptr && parseTier(envCap) >= 0) {
        effective = std::min(effective, parseTier(envCap));
    }
    report.tier = static_cast<u32>(effective);
    report.fromRendererCaps = false;
    report.summary = "T" + std::to_string(effective) + " (inferred from device extensions, hw T" +
                     std::to_string(hardware) + ")";
    return report;
}

} // namespace

std::unique_ptr<HeadlessFrameRunner> HeadlessFrameRunner::create(const HarnessOptions& options, std::string& reason) {
    std::unique_ptr<HeadlessFrameRunner> runner(new HeadlessFrameRunner());
    runner->m_options = options;
    runner->m_impl = std::make_unique<Impl>();
    Impl& impl = *runner->m_impl;
    impl.stockVert = options.shaderDir + "/gbuffer.vert.spv";
    impl.frag = options.shaderDir + "/gbuffer.frag.spv";
    impl.projectedVert = options.harnessShaderDir + "/harness_gbuffer.vert.spv";
    if (!fileExists(impl.stockVert) || !fileExists(impl.frag) || !fileExists(impl.projectedVert)) {
        reason = "raster SPIR-V missing (" + impl.stockVert + ", " + impl.projectedVert +
                 "; glslangValidator not found at configure time?)";
        return nullptr;
    }

    const bool validation = options.validation && !envDisabled("FUSE_HARNESS_VALIDATION");
    if (validation) {
        // Layer settings through the environment (VK_<LAYER>_<SETTING>); callers may override.
        setEnvIfUnset("VK_KHRONOS_VALIDATION_VALIDATE_SYNC", "true");
        setEnvIfUnset("VK_LAYER_ENABLES", "VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT");
        setEnvIfUnset("VK_KHRONOS_VALIDATION_DEBUG_ACTION", "VK_DBG_LAYER_ACTION_CALLBACK");
    }

    VulkanBootstrapDesc desc{};
    desc.instance.appName = "fuse_rp_harness";
    desc.instance.enableValidation = validation;
    desc.createSwapchain = false;
#if defined(FUSE_RP_HARNESS_RENDERER_CAPS)
    if (options.tierCap >= 0) {
        desc.device.maxTier = static_cast<RenderTier>(std::min(options.tierCap, 3));
    }
#endif
    impl.bootstrap = VulkanBootstrap::create(desc);
    if (impl.bootstrap == nullptr || !impl.bootstrap->status().deviceReady || impl.bootstrap->frameManager() == nullptr ||
        !impl.bootstrap->frameManager()->isReady()) {
        reason = "no Vulkan device (needs an ICD, Lavapipe in CI)";
        if (impl.bootstrap != nullptr) {
            reason += ": " + impl.bootstrap->status().message;
        }
        return nullptr;
    }
    for (const char* layer : impl.bootstrap->instance()->info().enabledLayers) {
        if (std::strcmp(layer, "VK_LAYER_KHRONOS_validation") == 0) {
            runner->m_validationRequested = true;
        }
    }
    const char* sync = std::getenv("VK_KHRONOS_VALIDATION_VALIDATE_SYNC");
    runner->m_syncRequested = runner->m_validationRequested && sync != nullptr &&
                              (std::strcmp(sync, "true") == 0 || std::strcmp(sync, "1") == 0);

    VulkanDevice& device = *impl.bootstrap->device();
#if defined(FUSE_RP_HARNESS_RENDERER_CAPS)
    const RendererCaps& caps = device.info().caps;
    if (caps.valid) {
        u32 tier = static_cast<u32>(caps.tier);
        if (options.tierCap >= 0) {
            tier = std::min(tier, static_cast<u32>(options.tierCap));
        }
        runner->m_tier.tier = tier;
        runner->m_tier.fromRendererCaps = true;
        runner->m_tier.summary = caps.summary();
    } else {
        runner->m_tier = inferTier(*impl.bootstrap, options.tierCap);
    }
#else
    runner->m_tier = inferTier(*impl.bootstrap, options.tierCap);
#endif

    impl.bindless.init(device);
    impl.bindlessReady = true;
    impl.resourcesReady = impl.resources.init(device, impl.bindless);
    if (!impl.resourcesReady) {
        reason = "ResourceManager init failed";
        return nullptr;
    }
    GBufferDesc gdesc{};
    gdesc.width = options.width;
    gdesc.height = options.height;
    impl.gbufferReady = impl.gbuffer.init(impl.resources, gdesc);
    if (!impl.gbufferReady) {
        reason = "G-buffer allocation failed";
        return nullptr;
    }
    return runner;
}

HeadlessFrameRunner::~HeadlessFrameRunner() = default;

std::string HeadlessFrameRunner::deviceName() const {
    return m_impl->bootstrap->device()->info().deviceName;
}

ValidationReport HeadlessFrameRunner::validation() const {
    ValidationReport r;
    r.layerRequested = m_validationRequested;
    r.syncRequested = m_syncRequested;
    const VulkanValidationCounters counters = vulkanValidationCounters();
    r.errors = counters.errors;
    r.warnings = counters.warnings;
    r.lastError = counters.lastError;
    return r;
}

void HeadlessFrameRunner::resetValidation() {
    resetVulkanValidationCounters();
}

u32 HeadlessFrameRunner::runSyncHazardControl() {
    VulkanDevice& device = *m_impl->bootstrap->device();
    const u32 before = vulkanValidationCounters().errors;
    BufferDesc desc{};
    desc.size = 4096;
    desc.usage = static_cast<BufferUsage>(static_cast<u32>(BufferUsage::TransferDst) | static_cast<u32>(BufferUsage::Storage));
    desc.memoryUsage = MemoryUsage::GpuOnly;
    desc.name = "fuse.rp_harness.hazard_control";
    const BufferHandle handle = m_impl->resources.createBuffer(desc);
    Buffer* buffer = m_impl->resources.getBuffer(handle);
    if (buffer == nullptr || buffer->handle == nullptr) {
        return 0;
    }
    const auto vkDevice = static_cast<VkDevice>(device.nativeHandle());
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.queueFamilyIndex = device.queues().graphicsFamily;
    VkCommandPool pool = VK_NULL_HANDLE;
    vkCreateCommandPool(vkDevice, &poolInfo, nullptr, &pool);
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vkDevice, &allocInfo, &cmd);
    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(cmd, &begin);
    vkCmdFillBuffer(cmd, static_cast<VkBuffer>(buffer->handle), 0, VK_WHOLE_SIZE, 1u);
    vkCmdFillBuffer(cmd, static_cast<VkBuffer>(buffer->handle), 0, VK_WHOLE_SIZE, 2u); // no barrier: WAW
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    const auto queue = static_cast<VkQueue>(device.queues().graphics);
    vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    vkDestroyCommandPool(vkDevice, pool, nullptr);
    m_impl->resources.destroyBuffer(handle);
    return vulkanValidationCounters().errors - before;
}

bool HeadlessFrameRunner::render(const Scene& scene, RasterShaders shaders, FrameCapture& out, std::string& error) {
    Impl& impl = *m_impl;
    if (shaders == RasterShaders::Stock) {
        for (const Batch& b : scene.batches) {
            if (!b.screenSpace) {
                error = "stock gbuffer.vert takes per-draw depth: only screen-space scenes (use Projected)";
                return false;
            }
        }
    }
    GBufferRasterPass* pass = impl.pass(shaders, error);
    if (pass == nullptr) {
        return false;
    }
    VulkanDevice& device = *impl.bootstrap->device();
    FrameManager& frames = *impl.bootstrap->frameManager();
    const u32 w = m_options.width;
    const u32 h = m_options.height;

    auto t0 = std::chrono::steady_clock::now();
    const ProjectedScene projected = projectScene(scene, static_cast<f32>(w) / static_cast<f32>(h));
    std::vector<GBufferDraw> draws;
    draws.reserve(scene.batches.size());
    for (usize i = 0; i < scene.batches.size(); ++i) {
        const Batch& b = scene.batches[i];
        if (projected.vertexCount[i] == 0u) {
            continue;
        }
        const SurfaceMaterial& m = scene.materials.at(b.material);
        GBufferDraw d{};
        d.firstVertex = projected.firstVertex[i];
        d.vertexCount = projected.vertexCount[i];
        d.push.albedo[0] = m.albedo.x;
        d.push.albedo[1] = m.albedo.y;
        d.push.albedo[2] = m.albedo.z;
        d.push.albedo[3] = 1.f;
        const Vec3 n = b.normal.normalized();
        d.push.normal[0] = n.x;
        d.push.normal[1] = n.y;
        d.push.normal[2] = n.z;
        d.push.surface[0] = m.roughness;
        d.push.surface[1] = m.metallic;
        d.push.surface[2] = b.screenSpace ? b.screenDepth : 0.5f;
        d.push.surface[3] = m.ao;
        d.push.emissive[0] = m.emissive.x;
        d.push.emissive[1] = m.emissive.y;
        d.push.emissive[2] = m.emissive.z;
        d.push.emissive[3] = static_cast<f32>(m.shadingModel);
        draws.push_back(d);
    }
    out = FrameCapture{};
    out.width = w;
    out.height = h;
    out.draws = static_cast<u32>(draws.size());
    out.vertices = projected.vertices.size() / 3u;
    out.triangles = out.vertices / 3u;
    out.projectMs = msSince(t0);

    BufferDesc vbDesc{};
    vbDesc.size = std::max<usize>(projected.vertices.size() * sizeof(f32), 64u);
    vbDesc.usage = BufferUsage::Vertex;
    vbDesc.memoryUsage = MemoryUsage::CpuToGpu;
    vbDesc.name = "fuse.rp_harness.vertices";
    const BufferHandle vb = impl.resources.createBuffer(vbDesc);
    Buffer* vertexBuffer = impl.resources.getBuffer(vb);
    if (vertexBuffer == nullptr || vertexBuffer->mapped == nullptr) {
        error = "host-visible vertex buffer allocation failed";
        if (vb.isValid()) {
            impl.resources.destroyBuffer(vb);
        }
        return false;
    }
    if (!projected.vertices.empty()) {
        std::memcpy(vertexBuffer->mapped, projected.vertices.data(), projected.vertices.size() * sizeof(f32));
    }

    t0 = std::chrono::steady_clock::now();
    bool ok = true;
    frames.signalTickComplete();
    frames.beginFrame(impl.frameCounter++);
    ok = resetFrameSlotCommandPool(device, frames);
    auto cmd = static_cast<VkCommandBuffer>(frames.currentCommandBuffer());
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ok = ok && vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS;
    ok = ok && pass->record(cmd, vertexBuffer->handle, draws.data(), static_cast<u32>(draws.size()));
    ok = ok && vkEndCommandBuffer(cmd) == VK_SUCCESS;
    if (ok) {
        GraphicsQueueSubmitDesc submitDesc{};
        submitDesc.device = &device;
        submitDesc.frameManager = &frames;
        submitDesc.commandsAlreadyRecorded = true;
        const GraphicsQueueSubmitResult submit = submitGraphicsQueue(submitDesc);
        ok = submit.ok && submit.submitted;
    }
    frames.endFrame();
    device.waitIdle();
    out.gpuFrameMs = msSince(t0);
    impl.resources.destroyBuffer(vb);
    if (!ok) {
        error = "G-buffer frame record / submit failed";
        return false;
    }

    t0 = std::chrono::steady_clock::now();
    const usize px = static_cast<usize>(w) * h;
    std::vector<u16> rt0(px * 4u);
    std::vector<u8> rt1(px * 4u);
    std::vector<u8> rt2(px * 4u);
    std::vector<f32> rt4(px);
    std::vector<u16> rt5(px * 4u);
    const GBufferTargets& targets = impl.gbuffer.targets();
    auto target = [&targets](GBufferAttachment a) { return targets.attachments[static_cast<u32>(a)]; };
    ok = impl.resources.readTexture(target(GBufferAttachment::NormalAo), rt0.data(), rt0.size() * sizeof(u16)) &&
         impl.resources.readTexture(target(GBufferAttachment::AlbedoAlpha), rt1.data(), rt1.size()) &&
         impl.resources.readTexture(target(GBufferAttachment::RoughMetalEmissiveShading), rt2.data(), rt2.size()) &&
         impl.resources.readTexture(target(GBufferAttachment::Depth), rt4.data(), rt4.size() * sizeof(f32)) &&
         impl.resources.readTexture(target(GBufferAttachment::Emissive), rt5.data(), rt5.size() * sizeof(u16));
    out.readbackMs = msSince(t0);
    if (!ok) {
        error = "G-buffer readback failed";
        return false;
    }

    auto half = [](u16 bits) { return GBufferQuantize::halfToFloat(bits); };
    out.normal.resize(px);
    out.ao.resize(px);
    out.albedo.resize(px);
    out.surface.resize(px);
    out.depth = std::move(rt4);
    out.emissive.resize(px);
    for (usize i = 0; i < px; ++i) {
        out.albedo[i] = Vec4(rt1[i * 4] / 255.f, rt1[i * 4 + 1] / 255.f, rt1[i * 4 + 2] / 255.f, rt1[i * 4 + 3] / 255.f);
        out.surface[i] = Vec4(rt2[i * 4] / 255.f, rt2[i * 4 + 1] / 255.f, rt2[i * 4 + 2] / 255.f, rt2[i * 4 + 3] / 255.f);
        out.ao[i] = half(rt0[i * 4 + 3]);
        out.normal[i] = out.albedo[i].w > 0.f
                            ? GBufferEncoding::decodeOctSigned(math::Vec2(half(rt0[i * 4]), half(rt0[i * 4 + 1])))
                            : Vec3(0.f, 0.f, 0.f);
        out.emissive[i] = Vec3(half(rt5[i * 4]), half(rt5[i * 4 + 1]), half(rt5[i * 4 + 2]));
    }
    resolveCapture(out, scene.camera.eye - scene.camera.target);
    return true;
}

#else // !FUSE_VULKAN_BACKEND

struct HeadlessFrameRunner::Impl {};

std::unique_ptr<HeadlessFrameRunner> HeadlessFrameRunner::create(const HarnessOptions&, std::string& reason) {
    reason = "Vulkan backend disabled (stub build)";
    return nullptr;
}

HeadlessFrameRunner::~HeadlessFrameRunner() = default;

std::string HeadlessFrameRunner::deviceName() const {
    return {};
}

ValidationReport HeadlessFrameRunner::validation() const {
    return {};
}

void HeadlessFrameRunner::resetValidation() {}

u32 HeadlessFrameRunner::runSyncHazardControl() {
    return 0;
}

bool HeadlessFrameRunner::render(const Scene&, RasterShaders, FrameCapture&, std::string& error) {
    error = "Vulkan backend disabled";
    return false;
}

#endif

} // namespace fuse::renderer::harness
