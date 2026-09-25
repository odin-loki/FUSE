// WP-4.2: "fsr3" behind ITemporalUpscaler (see fsr3_upscaler.hpp).
#include <fuse/renderer/upscale_backends/fsr3/fsr3_upscaler.hpp>

#include <fuse/renderer/rg/executor.hpp>
#include <fuse/renderer/rg/graph.hpp>
#include <fuse/renderer/vk/allocator.hpp>
#include <fuse/renderer/vk/device.hpp>

#include <cmath>
#include <cstring>
#include <mutex>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace fuse::renderer::fsr3 {

namespace {
std::mutex g_bindingMutex;
Fsr3BackendBinding g_binding{};

#if defined(FUSE_VULKAN_BACKEND)
std::unique_ptr<upscale::IUpscaler> makeFsr3() {
    Fsr3BackendBinding b{};
    {
        const std::lock_guard<std::mutex> lock(g_bindingMutex);
        b = g_binding;
    }
    if (b.device == nullptr || b.allocator == nullptr || b.executor == nullptr) {
        return nullptr;
    }
    return std::make_unique<Fsr3TemporalUpscaler>(b);
}

f32 halfToFloat(u16 h) {
    const u32 sign = (h >> 15u) & 1u;
    const u32 exponent = (h >> 10u) & 0x1Fu;
    const u32 mantissa = h & 0x3FFu;
    f32 v = 0.f;
    if (exponent == 0u) {
        v = std::ldexp(static_cast<f32>(mantissa), -24);
    } else if (exponent == 31u) {
        v = mantissa == 0u ? INFINITY : NAN;
    } else {
        v = std::ldexp(static_cast<f32>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
    }
    return sign != 0u ? -v : v;
}

constexpr u32 kFormatRgba32f = 109u; // VK_FORMAT_R32G32B32A32_SFLOAT
constexpr u32 kFormatR32f = 100u;    // VK_FORMAT_R32_SFLOAT

struct UploadRecord {
    rg::TextureRef image;
    rg::BufferRef src;
    u64 offset = 0;
    u32 width = 0;
    u32 height = 0;
};

void recordUpload(const rg::PassContext& pc, void* user) {
    const UploadRecord& u = *static_cast<const UploadRecord*>(user);
    VkBufferImageCopy region{};
    region.bufferOffset = u.offset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {u.width, u.height, 1};
    vkCmdCopyBufferToImage(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkBuffer>(pc.buffer(u.src)),
                           static_cast<VkImage>(pc.image(u.image)), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

struct ReadbackRecord {
    rg::TextureRef image;
    rg::BufferRef dst;
    u32 width = 0;
    u32 height = 0;
};

void recordReadback(const rg::PassContext& pc, void* user) {
    const ReadbackRecord& r = *static_cast<const ReadbackRecord*>(user);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {r.width, r.height, 1};
    vkCmdCopyImageToBuffer(static_cast<VkCommandBuffer>(pc.commandBuffer), static_cast<VkImage>(pc.image(r.image)),
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, static_cast<VkBuffer>(pc.buffer(r.dst)), 1, &region);
}
#endif
} // namespace

upscale::UpscalerCaps fsr3_caps() {
    upscale::UpscalerCaps c{};
    c.name = upscale::kFsr3Name;
    c.display_name = "AMD FidelityFX Super Resolution 3.1 (upscaler)";
    c.license = "MIT";
    c.kind = upscale::UpscalerKind::Temporal;
    c.temporal = true;
    c.needs_depth = true;
    c.needs_motion_vectors = true;
    c.needs_jitter = true;
    c.needs_exposure = true; // validateUpscaleInputs requires exposure > 0; FSR reads it as the exposure texture
    c.accepts_exposure = true;
    c.accepts_reactive_mask = true;
    c.accepts_transparency_mask = true;
    c.hdr_input = true;
    c.built_in_sharpening = true; // RCAS
    c.supports_dynamic_resolution = true;
    c.min_ratio = 1.f;
    c.max_ratio = 3.f;
    c.quality_modes = upscale::kAllQualityModes;
    c.apis = upscale::api_bit(upscale::UpscalerApi::Vulkan);
    return c;
}

bool register_fsr3_backend(upscale::UpscalerRegistry& registry, const Fsr3BackendBinding& binding) {
#if defined(FUSE_VULKAN_BACKEND) && defined(FUSE_UPSCALER_FSR3)
    if (binding.device == nullptr || binding.allocator == nullptr || binding.executor == nullptr ||
        !queryFsr3Capabilities(binding.device).supported || registry.find(upscale::kFsr3Name) != nullptr) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(g_bindingMutex);
        g_binding = binding;
    }
    return registry.register_backend(fsr3_caps(), &makeFsr3);
#else
    // Stub backend, or FUSE_UPSCALER_FSR3=OFF (cmake/upscale.cmake): "fsr3" is never registered.
#if defined(FUSE_VULKAN_BACKEND)
    (void)&makeFsr3;
#endif
    (void)registry;
    (void)binding;
    return false;
#endif
}

void unregister_fsr3_backend(upscale::UpscalerRegistry& registry) {
    registry.unregister_backend(upscale::kFsr3Name);
    const std::lock_guard<std::mutex> lock(g_bindingMutex);
    g_binding = Fsr3BackendBinding{};
}

// ---- adapter --------------------------------------------------------------------------------------------

struct Fsr3TemporalUpscaler::Staging {
    struct Img {
        Texture tex{};
        u32 layout = 0;
        u8 queue = rg::kNoQueue;
    };
    struct Buf {
        Buffer buf{};
        u8 queue = rg::kNoQueue;
    };
    UpscaleResolution res{};
    Img color, reactive, transparency;
    Buf upload, depth, motion, readback;
    rg::Graph graph;
};

Fsr3TemporalUpscaler::Fsr3TemporalUpscaler(const Fsr3BackendBinding& binding) : m_binding(binding), m_caps(fsr3_caps()) {}

Fsr3TemporalUpscaler::~Fsr3TemporalUpscaler() {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_binding.executor != nullptr) {
        m_binding.executor->waitIdle();
    }
#endif
    releaseStaging();
    m_gpu.destroy();
}

void Fsr3TemporalUpscaler::invalidate_history(upscale::HistoryResetReason reason) {
    m_pendingReset = true;
    m_accumulated = 0;
    ++m_generation;
    m_lastReason = reason;
}

void Fsr3TemporalUpscaler::releaseStaging() {
    if (m_staging == nullptr || m_binding.allocator == nullptr) {
        m_staging.reset();
        return;
    }
    Staging& s = *m_staging;
    for (Staging::Img* i : {&s.color, &s.reactive, &s.transparency}) {
        if (i->tex.image != nullptr) {
            m_binding.allocator->destroyImage(i->tex);
        }
    }
    for (Staging::Buf* b : {&s.upload, &s.depth, &s.motion, &s.readback}) {
        if (b->buf.handle != nullptr) {
            m_binding.allocator->destroyBuffer(b->buf);
        }
    }
    m_staging.reset();
}

bool Fsr3TemporalUpscaler::ensureStaging(const UpscaleResolution& res) {
#if defined(FUSE_VULKAN_BACKEND)
    if (m_staging != nullptr && m_staging->res.render_width == res.render_width &&
        m_staging->res.render_height == res.render_height && m_staging->res.display_width == res.display_width &&
        m_staging->res.display_height == res.display_height) {
        return true;
    }
    if (m_binding.executor != nullptr) {
        m_binding.executor->waitIdle();
    }
    releaseStaging();
    m_staging = std::make_unique<Staging>();
    Staging& s = *m_staging;
    s.res = res;
    const u64 rn = static_cast<u64>(res.render_width) * res.render_height;
    const u64 dn = static_cast<u64>(res.display_width) * res.display_height;
    auto image = [&](Staging::Img& img, u32 format, const char* name) {
        TextureDesc d{};
        d.width = res.render_width;
        d.height = res.render_height;
        d.format = static_cast<GpuFormat>(format);
        d.usage = static_cast<ImageUsage>(static_cast<u32>(ImageUsage::Sampled) | static_cast<u32>(ImageUsage::TransferDst));
        d.name = name;
        return m_binding.allocator->createImage(d, img.tex);
    };
    auto buffer = [&](Staging::Buf& b, u64 bytes, BufferUsage usage, MemoryUsage memory, const char* name) {
        BufferDesc d{};
        d.size = static_cast<usize>(bytes);
        d.usage = usage;
        d.memoryUsage = memory;
        d.name = name;
        return m_binding.allocator->createBuffer(d, b.buf) && b.buf.mapped != nullptr;
    };
    const bool ok = image(s.color, kFormatRgba32f, "fsr3.adapter.color") && image(s.reactive, kFormatR32f, "fsr3.adapter.reactive") &&
                    image(s.transparency, kFormatR32f, "fsr3.adapter.transparency") &&
                    buffer(s.upload, rn * 24u, BufferUsage::TransferSrc, MemoryUsage::CpuToGpu, "fsr3.adapter.upload") &&
                    buffer(s.depth, rn * 4u, BufferUsage::Storage, MemoryUsage::CpuToGpu, "fsr3.adapter.depth") &&
                    buffer(s.motion, rn * 8u, BufferUsage::Storage, MemoryUsage::CpuToGpu, "fsr3.adapter.motion") &&
                    buffer(s.readback, dn * 8u, BufferUsage::TransferDst, MemoryUsage::GpuToCpu, "fsr3.adapter.readback");
    if (!ok) {
        releaseStaging();
    }
    return ok;
#else
    (void)res;
    return false;
#endif
}

upscale::UpscaleStatus Fsr3TemporalUpscaler::evaluate(const upscale::UpscaleDispatch& dispatch, const UpscaleInputs& inputs,
                                                     const upscale::TemporalUpscaleOutputs& outputs) {
    (void)dispatch; // Vulkan only: the kernel backend selector does not apply
    const upscale::UpscaleStatus status = validate(inputs, outputs);
    if (status != upscale::UpscaleStatus::Ok) {
        return status;
    }
#if defined(FUSE_VULKAN_BACKEND)
    if (m_binding.device == nullptr || m_binding.allocator == nullptr || m_binding.executor == nullptr) {
        return upscale::UpscaleStatus::BackendUnavailable;
    }
    const UpscaleResolution& r = inputs.resolution;
    if (!m_gpu.valid()) {
        Fsr3GpuDesc d{};
        d.device = m_binding.device;
        d.allocator = m_binding.allocator;
        d.maxResolution = r;
        d.framesInFlight = 2;
        d.language = m_language;
        if (!m_gpu.init(d)) {
            return upscale::UpscaleStatus::BackendUnavailable;
        }
    }
    const bool resized = r.render_width != m_lastResolution.render_width || r.render_height != m_lastResolution.render_height ||
                         r.display_width != m_lastResolution.display_width ||
                         r.display_height != m_lastResolution.display_height;
    if (resized) {
        if (m_lastResolution.valid()) {
            invalidate_history(upscale::HistoryResetReason::ResolutionChange);
        }
        m_lastResolution = r;
        m_pendingReset = true;
    } else if (inputs.reset_history) {
        invalidate_history(upscale::HistoryResetReason::CameraCut);
    }
    if (!ensureStaging(r)) {
        return upscale::UpscaleStatus::LaunchFailed;
    }
    Staging& s = *m_staging;
    const u64 rn = inputs.renderPixelCount();
    const u64 dn = inputs.displayPixelCount();
    f32* up = static_cast<f32*>(s.upload.buf.mapped);
    for (u64 i = 0; i < rn; ++i) {
        up[i * 4u + 0u] = inputs.color[i].x;
        up[i * 4u + 1u] = inputs.color[i].y;
        up[i * 4u + 2u] = inputs.color[i].z;
        up[i * 4u + 3u] = 1.f;
    }
    const bool hasReactive = inputs.reactive != nullptr;
    const bool hasTransparency = inputs.transparency_composition != nullptr;
    if (hasReactive) {
        std::memcpy(up + rn * 4u, inputs.reactive, rn * 4u);
    }
    if (hasTransparency) {
        std::memcpy(up + rn * 5u, inputs.transparency_composition, rn * 4u);
    }
    std::memcpy(s.depth.buf.mapped, inputs.depth, rn * 4u);
    std::memcpy(s.motion.buf.mapped, inputs.motion, rn * 8u);

    Fsr3GpuFrameDesc fd{};
    fd.resolution = r;
    fd.jitter_px = inputs.jitter_px;
    fd.exposure = inputs.exposure;
    fd.frame_time_s = inputs.frame_time_s;
    fd.reset_history = m_pendingReset || inputs.reset_history;
    fd.camera = inputs.camera;
    fd.sharpen = m_sharpness > 0.f;
    fd.sharpness = m_sharpness;
    fd.debug_flip_jitter_sign = m_flipJitter;
    ++m_serial;
    if (!m_gpu.beginFrame(m_serial, fd)) {
        return upscale::UpscaleStatus::InvalidInputs;
    }
    m_pendingReset = false;

    rg::Graph& g = s.graph;
    g.reset();
    const rg::BufferRef upload = g.importBuffer(rg::ImportedBuffer{s.upload.buf.handle, s.upload.buf.desc.size, s.upload.queue,
                                                                   &s.upload.queue, "fsr3.adapter.upload"});
    Staging::Img* imgs[3] = {&s.color, &s.reactive, &s.transparency};
    const bool used[3] = {true, hasReactive, hasTransparency};
    const u32 formats[3] = {kFormatRgba32f, kFormatR32f, kFormatR32f};
    const u64 offsets[3] = {0u, rn * 16u, rn * 20u};
    UploadRecord ups[3];
    rg::TextureRef refs[3];
    for (u32 k = 0; k < 3u; ++k) {
        if (!used[k]) {
            continue;
        }
        rg::ImportedImage ii{};
        ii.image = imgs[k]->tex.image;
        ii.view = imgs[k]->tex.view;
        ii.format = formats[k];
        ii.width = r.render_width;
        ii.height = r.render_height;
        ii.initialLayout = imgs[k]->layout;
        ii.initialQueue = imgs[k]->queue;
        ii.layoutTracker = &imgs[k]->layout;
        ii.queueTracker = &imgs[k]->queue;
        ii.name = "fsr3.adapter.input";
        refs[k] = g.importImage(ii);
        ups[k] = UploadRecord{refs[k], upload, offsets[k], r.render_width, r.render_height};
        g.addPass("fsr3.adapter.upload", &recordUpload, &ups[k])
            .use(refs[k], rg::Access::TransferDst)
            .use(upload, rg::Access::TransferSrc, rg::BufferRange{offsets[k], k == 0u ? rn * 16u : rn * 4u});
    }
    Fsr3GpuInputs in{};
    in.color = refs[0];
    in.reactive = hasReactive ? refs[1] : rg::TextureRef{};
    in.transparency = hasTransparency ? refs[2] : rg::TextureRef{};
    in.depth = g.importBuffer(rg::ImportedBuffer{s.depth.buf.handle, s.depth.buf.desc.size, s.depth.queue, &s.depth.queue,
                                                 "fsr3.adapter.depth"});
    in.motion = g.importBuffer(rg::ImportedBuffer{s.motion.buf.handle, s.motion.buf.desc.size, s.motion.queue, &s.motion.queue,
                                                  "fsr3.adapter.motion"});
    const Fsr3GraphRefs fr = m_gpu.importInto(g);
    m_gpu.addPasses(g, fr, in);
    const rg::BufferRef rb = g.importBuffer(rg::ImportedBuffer{s.readback.buf.handle, s.readback.buf.desc.size, s.readback.queue,
                                                               &s.readback.queue, "fsr3.adapter.readback"});
    ReadbackRecord rr{fr.output, rb, r.display_width, r.display_height};
    g.addPass("fsr3.adapter.readback", &recordReadback, &rr).use(fr.output, rg::Access::TransferSrc).use(rb, rg::Access::TransferDst);
    g.addPass("fsr3.adapter.host", nullptr, nullptr).use(rb, rg::Access::HostRead);
    const bool ok = m_binding.executor->execute(g).ok && m_binding.executor->waitIdle();
    m_gpu.collectRetired(m_serial);
    if (!ok) {
        return upscale::UpscaleStatus::LaunchFailed;
    }
    const u16* half = static_cast<const u16*>(s.readback.buf.mapped);
    for (u64 i = 0; i < dn; ++i) {
        outputs.color.data[i] = math::Vec3(halfToFloat(half[i * 4u + 0u]), halfToFloat(half[i * 4u + 1u]), halfToFloat(half[i * 4u + 2u]));
    }
    ++m_accumulated;
    return upscale::UpscaleStatus::Ok;
#else
    (void)inputs;
    (void)outputs;
    return upscale::UpscaleStatus::BackendUnavailable;
#endif
}

} // namespace fuse::renderer::fsr3
