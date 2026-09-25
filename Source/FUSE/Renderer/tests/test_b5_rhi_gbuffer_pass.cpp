// B2 gate row: "G-buffer pass populates normal, albedo, depth attachments correctly — verified with
// RenderDoc".
//
// GBufferRasterPass (6 MRT targets from GBuffer + D32 depth, shaders/raster/gbuffer.{vert,frag}
// compiled at build time, write_gbuffer() from shaders/common/gbuffer.glsl) renders two
// overlapping quads with different surfaces through a FrameManager slot submit. Draw order is
// near-then-far, so the overlap also proves the depth test. Every attachment is read back
// through ResourceManager::readTexture and compared per region against the CPU mirror
// (GBufferPacking::pack + quantizeToStorage):
//   RT0 normal (signed oct, RGBA16F) + AO, RT1 albedo (RGBA8), RT4 depth (R32F) — the row's
//   three attachments — plus RT2 roughness/metallic/emissive-mask/shading, RT3 velocity, RT5
//   emissive. Uncovered pixels keep the clear (0) everywhere.
#include "b5_rhi_test_common.hpp"

#include <fuse/core/init.hpp>
#include <fuse/renderer/deferred/gbuffer.hpp>
#include <fuse/renderer/deferred/gbuffer_raster_pass.hpp>
#include <fuse/renderer/resource_manager.hpp>
#include <fuse/renderer/vk/bindless.hpp>
#include <fuse/renderer/vk/bootstrap.hpp>
#include <fuse/renderer/vk/queue_submit.hpp>

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#if defined(FUSE_VULKAN_BACKEND)
#include <vulkan/vulkan.h>
#endif

namespace {

using b5rhi::expectTrue;
using fuse::f32;
using fuse::u16;
using fuse::u32;
using fuse::u8;
using fuse::math::Vec3;
using fuse::math::Vec4;
using namespace fuse::renderer;

constexpr u32 kW = 64;
constexpr u32 kH = 64;

struct Surface {
    GBufferPackedData data;
    f32 depth = 0.5f;
};

[[maybe_unused]] GBufferDrawPush toPush(const Surface& s) {
    GBufferDrawPush push{};
    push.albedo[0] = s.data.albedo.x;
    push.albedo[1] = s.data.albedo.y;
    push.albedo[2] = s.data.albedo.z;
    push.normal[0] = s.data.normal.x;
    push.normal[1] = s.data.normal.y;
    push.normal[2] = s.data.normal.z;
    push.surface[0] = s.data.roughness;
    push.surface[1] = s.data.metallic;
    push.surface[2] = s.depth;
    push.surface[3] = s.data.ao;
    push.emissive[0] = s.data.emissive.x;
    push.emissive[1] = s.data.emissive.y;
    push.emissive[2] = s.data.emissive.z;
    push.emissive[3] = static_cast<f32>(s.data.shadingModel);
    return push;
}

/// Decoded texel of every attachment at one pixel.
struct Texels {
    Vec4 rt0, rt1, rt2, rt3, rt5;
    f32 depth = 0.f;
};

struct Readback {
    std::vector<u16> rt0;  // RGBA16F
    std::vector<u8> rt1;   // RGBA8
    std::vector<u8> rt2;   // RGBA8
    std::vector<u16> rt3;  // RG16F
    std::vector<f32> rt4;  // R32F
    std::vector<u16> rt5;  // RGBA16F

    Texels at(u32 x, u32 y) const {
        const std::size_t p = static_cast<std::size_t>(y) * kW + x;
        auto h = [](u16 bits) { return GBufferQuantize::halfToFloat(bits); };
        auto n = [](u8 v) { return static_cast<f32>(v) / 255.f; };
        Texels t;
        t.rt0 = {h(rt0[p * 4]), h(rt0[p * 4 + 1]), h(rt0[p * 4 + 2]), h(rt0[p * 4 + 3])};
        t.rt1 = {n(rt1[p * 4]), n(rt1[p * 4 + 1]), n(rt1[p * 4 + 2]), n(rt1[p * 4 + 3])};
        t.rt2 = {n(rt2[p * 4]), n(rt2[p * 4 + 1]), n(rt2[p * 4 + 2]), n(rt2[p * 4 + 3])};
        t.rt3 = {h(rt3[p * 2]), h(rt3[p * 2 + 1]), 0.f, 0.f};
        t.depth = rt4[p];
        t.rt5 = {h(rt5[p * 4]), h(rt5[p * 4 + 1]), h(rt5[p * 4 + 2]), h(rt5[p * 4 + 3])};
        return t;
    }
};

bool near4(const Vec4& a, const Vec4& b, f32 tol) {
    return std::fabs(a.x - b.x) <= tol && std::fabs(a.y - b.y) <= tol && std::fabs(a.z - b.z) <= tol &&
           std::fabs(a.w - b.w) <= tol;
}

[[maybe_unused]] void checkRegion(const Readback& rb, u32 x, u32 y, const Surface& s, const char* label) {
    const Texels t = rb.at(x, y);
    const GBufferMrt expected = GBufferPacking::quantizeToStorage(GBufferPacking::pack(s.data));
    const Vec3 decoded = GBufferEncoding::decodeOctSigned({t.rt0.x, t.rt0.y});
    const f32 normalError = GBufferEncoding::angularErrorRadians(decoded, s.data.normal.normalized());
    std::printf("%s @(%u,%u): normal err %.2e rad, albedo (%.3f %.3f %.3f), depth %.4f, ao %.3f\n", label, x, y,
                normalError, t.rt1.x, t.rt1.y, t.rt1.z, t.depth, t.rt0.w);

    char message[160];
    std::snprintf(message, sizeof(message), "%s: RT0 normal decodes within 1e-3 rad", label);
    expectTrue(normalError < 1e-3f, message);
    std::snprintf(message, sizeof(message), "%s: RT0 equals the CPU packing mirror (oct normal + AO)", label);
    expectTrue(near4(t.rt0, expected.rt0, 1e-3f), message);
    std::snprintf(message, sizeof(message), "%s: RT1 albedo equals the CPU mirror (RGBA8)", label);
    expectTrue(near4(t.rt1, expected.rt1, 1.f / 255.f + 1e-4f), message);
    std::snprintf(message, sizeof(message), "%s: RT2 roughness/metallic/emissive/shading equal the CPU mirror", label);
    expectTrue(near4(t.rt2, expected.rt2, 1.f / 255.f + 1e-4f), message);
    std::snprintf(message, sizeof(message), "%s: RT3 velocity is zero", label);
    expectTrue(t.rt3.x == 0.f && t.rt3.y == 0.f, message);
    std::snprintf(message, sizeof(message), "%s: RT4 depth == draw depth", label);
    expectTrue(std::fabs(t.depth - s.depth) < 1e-6f, message);
    std::snprintf(message, sizeof(message), "%s: RT5 emissive equals the CPU mirror (RGBA16F)", label);
    expectTrue(near4(t.rt5, expected.rt5, 2e-3f), message);
}

#if defined(FUSE_VULKAN_BACKEND)
template <typename T>
bool readAttachment(ResourceManager& resources, TextureHandle handle, std::vector<T>& out, std::size_t elements) {
    out.assign(elements, T{});
    return resources.readTexture(handle, out.data(), out.size() * sizeof(T));
}
#endif

} // namespace

int main() {
#if !defined(FUSE_VULKAN_BACKEND)
    return b5rhi::skip("fuse_b5_rhi_gbuffer_pass", "Vulkan backend disabled");
#elif !defined(FUSE_B5_RHI_SHADERS_BUILT)
    return b5rhi::skip("fuse_b5_rhi_gbuffer_pass", "raster shaders not built (glslangValidator missing)");
#else
    fuse::core::initialize();
    VulkanBootstrapDesc bootstrapDesc{};
    bootstrapDesc.instance.enableValidation = false;
    bootstrapDesc.createSwapchain = false;
    auto bootstrap = VulkanBootstrap::create(bootstrapDesc);
    if (bootstrap == nullptr || !bootstrap->status().deviceReady || bootstrap->frameManager() == nullptr ||
        !bootstrap->frameManager()->isReady()) {
        bootstrap.reset();
        fuse::core::shutdown();
        return b5rhi::skip("fuse_b5_rhi_gbuffer_pass", "no Vulkan device (needs an ICD, Lavapipe in CI)");
    }
    VulkanDevice& device = *bootstrap->device();
    FrameManager& frames = *bootstrap->frameManager();

    int status = 0;
    {
        BindlessDescriptors bindless;
        bindless.init(device);
        ResourceManager resources;
        expectTrue(resources.init(device, bindless), "resource manager ready");

        GBuffer gbuffer;
        GBufferDesc gbufferDesc{};
        gbufferDesc.width = kW;
        gbufferDesc.height = kH;
        expectTrue(gbuffer.init(resources, gbufferDesc), "G-buffer targets allocated (6 attachments)");

        const std::string vert = std::string(FUSE_B5_RHI_SHADER_DIR) + "/gbuffer.vert.spv";
        const std::string frag = std::string(FUSE_B5_RHI_SHADER_DIR) + "/gbuffer.frag.spv";
        GBufferRasterPassDesc passDesc{};
        passDesc.vertexSpirvPath = vert.c_str();
        passDesc.fragmentSpirvPath = frag.c_str();
        GBufferRasterPass pass;
        const bool passReady = pass.init(device, resources, gbuffer, passDesc);
        expectTrue(passReady, "G-buffer raster pass ready (MRT render pass + pipeline + framebuffer)");
        if (!passReady) {
            std::fprintf(stderr, "  %s\n", pass.stats().message.c_str());
        }

        // Two quads (6 vertices each), NDC. Far quad A covers [-1,0.5]^2, near quad B [-0.5,1]^2.
        const f32 quads[] = {
            -1.f, -1.f, 0.f, 0.5f, -1.f, 0.f, 0.5f, 0.5f, 0.f, -1.f, -1.f, 0.f, 0.5f, 0.5f, 0.f, -1.f, 0.5f, 0.f,
            -0.5f, -0.5f, 0.f, 1.f, -0.5f, 0.f, 1.f, 1.f, 0.f, -0.5f, -0.5f, 0.f, 1.f, 1.f, 0.f, -0.5f, 1.f, 0.f,
        };
        BufferDesc vbDesc{};
        vbDesc.size = sizeof(quads);
        vbDesc.usage = BufferUsage::Vertex;
        vbDesc.memoryUsage = MemoryUsage::CpuToGpu;
        vbDesc.name = "fuse.test.gbuffer_quads";
        const BufferHandle vb = resources.createBuffer(vbDesc);
        Buffer* vertexBuffer = resources.getBuffer(vb);
        expectTrue(vertexBuffer != nullptr && vertexBuffer->mapped != nullptr, "host-visible vertex buffer");

        Surface farSurface;
        farSurface.data.normal = Vec3(0.f, 1.f, 0.f);
        farSurface.data.albedo = Vec3(0.2f, 0.4f, 0.6f);
        farSurface.data.roughness = 0.3f;
        farSurface.data.metallic = 0.f;
        farSurface.data.ao = 0.8f;
        farSurface.depth = 0.75f;
        Surface nearSurface;
        nearSurface.data.normal = Vec3(0.3f, -0.5f, 0.8f).normalized();
        nearSurface.data.albedo = Vec3(0.9f, 0.1f, 0.3f);
        nearSurface.data.roughness = 0.7f;
        nearSurface.data.metallic = 1.f;
        nearSurface.data.ao = 0.5f;
        nearSurface.data.emissive = Vec3(2.f, 1.f, 0.5f);
        nearSurface.data.shadingModel = 3;
        nearSurface.depth = 0.25f;

        if (passReady && vertexBuffer != nullptr && vertexBuffer->mapped != nullptr) {
            std::memcpy(vertexBuffer->mapped, quads, sizeof(quads));
            GBufferDraw draws[2];
            draws[0].vertexCount = 6;
            draws[0].firstVertex = 6; // near first: the farSurface quad must fail the depth test on overlap
            draws[0].push = toPush(nearSurface);
            draws[1].vertexCount = 6;
            draws[1].firstVertex = 0;
            draws[1].push = toPush(farSurface);

            frames.signalTickComplete();
            frames.beginFrame(0u);
            expectTrue(resetFrameSlotCommandPool(device, frames), "frame slot command pool reset");
            auto cmd = static_cast<VkCommandBuffer>(frames.currentCommandBuffer());
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            expectTrue(vkBeginCommandBuffer(cmd, &beginInfo) == VK_SUCCESS, "slot command buffer begins");
            expectTrue(pass.record(cmd, vertexBuffer->handle, draws, 2u), "G-buffer pass recorded");
            expectTrue(vkEndCommandBuffer(cmd) == VK_SUCCESS, "slot command buffer ends");

            GraphicsQueueSubmitDesc submitDesc{};
            submitDesc.device = &device;
            submitDesc.frameManager = &frames;
            submitDesc.commandsAlreadyRecorded = true;
            const GraphicsQueueSubmitResult submit = submitGraphicsQueue(submitDesc);
            expectTrue(submit.ok && submit.submitted, "G-buffer frame submitted");
            frames.endFrame();
            device.waitIdle();

            const GBufferTargets& targets = gbuffer.targets();
            Readback rb;
            const std::size_t px = static_cast<std::size_t>(kW) * kH;
            auto target = [&targets](GBufferAttachment a) { return targets.attachments[static_cast<u32>(a)]; };
            expectTrue(readAttachment(resources, target(GBufferAttachment::NormalAo), rb.rt0, px * 4),
                       "RT0 normal/AO readback");
            expectTrue(readAttachment(resources, target(GBufferAttachment::AlbedoAlpha), rb.rt1, px * 4),
                       "RT1 albedo readback");
            expectTrue(readAttachment(resources, target(GBufferAttachment::RoughMetalEmissiveShading), rb.rt2, px * 4),
                       "RT2 surface readback");
            expectTrue(readAttachment(resources, target(GBufferAttachment::Velocity), rb.rt3, px * 2),
                       "RT3 velocity readback");
            expectTrue(readAttachment(resources, target(GBufferAttachment::Depth), rb.rt4, px), "RT4 depth readback");
            expectTrue(readAttachment(resources, target(GBufferAttachment::Emissive), rb.rt5, px * 4),
                       "RT5 emissive readback");

            if (rb.rt0.size() == px * 4 && rb.rt4.size() == px) {
                // NDC -> pixel: x_px = (ndc + 1) / 2 * W. Only-farSurface (-0.8,-0.8), only-near (0.8,0.8),
                // overlap (0,0), uncovered (0.8,-0.8).
                checkRegion(rb, kW / 10u, kH / 10u, farSurface, "far-only");
                checkRegion(rb, (kW * 9u) / 10u, (kH * 9u) / 10u, nearSurface, "near-only");
                checkRegion(rb, kW / 2u, kH / 2u, nearSurface, "overlap (depth test keeps near)");

                const Texels empty = rb.at((kW * 9u) / 10u, kH / 10u);
                expectTrue(near4(empty.rt0, {}, 0.f) && near4(empty.rt1, {}, 0.f) && empty.depth == 0.f &&
                               near4(empty.rt5, {}, 0.f),
                           "uncovered pixel keeps the clear value in every attachment");

                u32 nearCount = 0;
                u32 farCount = 0;
                u32 clearCount = 0;
                for (std::size_t i = 0; i < px; ++i) {
                    if (rb.rt4[i] == 0.25f) {
                        ++nearCount;
                    } else if (rb.rt4[i] == 0.75f) {
                        ++farCount;
                    } else if (rb.rt4[i] == 0.f) {
                        ++clearCount;
                    }
                }
                std::printf("depth coverage: near %u, far %u, clear %u of %zu\n", nearCount, farCount, clearCount, px);
                // Near quad: 0.75 x 0.75 of the target; farSurface minus overlap: 0.75^2 - 0.5^2; rest clear.
                expectTrue(nearCount == (kW * 3u / 4u) * (kH * 3u / 4u), "near quad covers exactly 3/4 x 3/4");
                expectTrue(farCount == (kW * 3u / 4u) * (kH * 3u / 4u) - (kW / 2u) * (kH / 2u),
                           "far quad visible only outside the overlap");
                expectTrue(nearCount + farCount + clearCount == px, "every depth texel is near, far or clear");
            }
            expectTrue(pass.stats().drawsRecorded == 2u && pass.stats().pushConstantUpdates == 2u,
                       "two draws, one push-constant block each");
        }

        resources.destroyBuffer(vb);
        pass.destroy();
        gbuffer.destroy();
        resources.destroy();
        bindless.destroy(device);
    }

    bootstrap.reset();
    fuse::core::shutdown();
    (void)status;
    return b5rhi::finish("fuse_b5_rhi_gbuffer_pass");
#endif
}
